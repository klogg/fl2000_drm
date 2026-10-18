#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Bridge identities are data in enumerator.conf, never executable shell input.
set -eu
set -f

fail()
{
    printf 'fl2000-enumerate: %s\n' "$*" >&2
    exit 1
}

if [ "${1:-}" = --help ]; then
    printf '%s\n' 'Usage: fl2000-enumerate ADAPTER-SYSFS-PATH [CONFIG-FILE]' \
        'Requires i2ctransfer (i2c-tools), kmod, util-linux flock and base utilities.'
    exit 0
fi
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    fail 'Expected adapter path and optional config path'
fi
config=${2:-/etc/fl2000/enumerator.conf}
sysfs=${SYSFS_ROOT:-/sys}
locks=${LOCK_ROOT:-/run/lock}
adapter=$(readlink -f -- "$1") || fail 'Cannot resolve adapter path'
bus=${adapter##*/i2c-}
case "$bus" in
    ''|*[!0-9]*) fail 'Expected an i2c-N adapter path' ;;
    *) ;;
esac
[ "$adapter" = "$(readlink -f -- "$sysfs/bus/i2c/devices/i2c-$bus")" ] ||
    fail 'Adapter is not registered on the I2C bus'

# Validate the whole file before any module loading or I2C access.
records=$(awk '
function bad(message) {
    print FILENAME ":" FNR ": " message > "/dev/stderr"; failed=1; exit 1
}
function byte(value) { return value ~ /^0x[0-9a-fA-F][0-9a-fA-F]$/ }
function number(value) {
    value=tolower(value)
    return (index("0123456789abcdef",substr(value,3,1))-1)*16+index("0123456789abcdef",substr(value,4,1))-1
}
{
    sub(/[[:space:]]*#.*/, ""); sub(/^[[:space:]]+/, ""); sub(/[[:space:]]+$/, "")
    if (!NF) next
    if ($1=="adapter_name") {
        if (name!="") bad("duplicate adapter_name")
        sub(/^[^[:space:]]+[[:space:]]+/, ""); name=$0
        if (name!~/^[A-Za-z0-9 _-]+$/) bad("invalid adapter_name")
    } else if ($1=="bind_timeout") {
        if (timeout!="" || NF!=2 || $2!~/^[0-9]+$/ || $2<1 || $2>20)
            bad("bind_timeout must occur once, in the range 1..20 seconds")
        timeout=$2+0
    } else if ($1=="bridge") {
        if (NF!=6) bad("bridge needs type, module, driver, addresses and identity")
        for (i=2;i<=4;i++)
            if ($i!~/^[A-Za-z0-9_][A-Za-z0-9_-]*$/) bad("invalid identifier")
        if (length($2)>19) bad("I2C type exceeds 19 characters")
        n=split($5,addresses,",")
        for (i=1;i<=n;i++)
            if (!byte(addresses[i]) || number(addresses[i])<3 || number(addresses[i])>119)
                bad("invalid seven-bit address")
        n=split($6,checks,",")
        for (i=1;i<=n;i++) {
            if (split(checks[i],parts,":")!=3 || !byte(parts[1]) || !byte(parts[2]) ||
                !byte(parts[3]) || number(parts[2])==0) bad("invalid identity check")
        }
        bridges[++count]=$2 "|" $3 "|" $4 "|" $5 "|" $6
    } else bad("unknown directive " $1)
}
END {
    if (failed) exit 1
    if (name=="" || timeout=="" || !count) bad("missing settings or bridge records")
    print "adapter_name|" name; print "bind_timeout|" timeout
    for (i=1;i<=count;i++) print "bridge|" bridges[i]
}' "$config") || fail 'Invalid configuration'

command -v i2ctransfer >/dev/null 2>&1 || fail 'Install i2c-tools (i2ctransfer is required)'
mkdir -p -- "$locks"
exec 9>"$locks/fl2000-enumerate-i2c-$bus.lock"
flock -x 9

load_module()
{
    requested_module=$1
    module_name=$(printf '%s' "$requested_module" | tr '-' '_')
    [ -d "$sysfs/module/$module_name" ] || modprobe -- "$requested_module"
}

wait_for_binding()
{
    # Retry an ordinary probe failure once, without replacing the client.
    if [ ! -e "$client/driver" ] && [ -d "$sysfs/bus/i2c/drivers/$driver" ] &&
        ! printf '%s\n' "${client##*/}" > "$sysfs/bus/i2c/drivers_probe"; then
        printf 'fl2000-enumerate: reprobe failed for %s; waiting for binding\n' \
            "$client" >&2
    fi
    remaining=$timeout
    while :; do
        if [ ! -d "$adapter" ] || [ ! -d "$client" ]; then
            fail 'Adapter or client disappeared'
        fi
        if [ -e "$client/driver" ]; then
            bound_driver=$(readlink -f -- "$client/driver")
            [ "${bound_driver##*/}" = "$driver" ] || fail 'Client bound to another driver'
            printf 'Bound %s at %s on i2c-%s\n' "$type" "$address" "$bus"
            exit 0
        fi
        [ "$remaining" -gt 0 ] || break
        sleep 1
        remaining=$((remaining - 1))
    done
    fail "$client has not bound; preserving it for deferred probing or a later driver load"
}

# A here-document keeps exit/status and all state in this shell.
for phase in existing probe; do
    while IFS='|' read -r record type module driver addresses checks; do
        case "$record" in
            adapter_name)
                [ "$(cat "$adapter/name")" = "$type" ] || fail 'Unexpected adapter name'
                ;;
            bind_timeout) timeout=$type ;;
            bridge)
                saved_ifs=$IFS
                IFS=,
                for address in $addresses; do
                    client="$sysfs/bus/i2c/devices/$bus-$(printf '%04x' "$address")"
                    if [ -d "$client" ]; then
                        # Never probe an occupied address or remove another caller's client.
                        [ "$(cat "$client/name")" = "$type" ] || continue
                        [ -d "$sysfs/bus/i2c/drivers/$driver" ] || load_module "$module"
                        wait_for_binding
                    fi
                    [ "$phase" = probe ] || continue
                    [ -d "$sysfs/class/i2c-dev/i2c-$bus" ] || load_module i2c-dev
                    matched=1
                    for check in $checks; do
                        register=${check%%:*}
                        rest=${check#*:}
                        mask=${rest%%:*}
                        expected=${rest#*:}
                        if ! value=$(i2ctransfer -y "$bus" "w1@$address" "$register" r1); then
                            matched=0
                            break
                        fi
                        case "$value" in
                            0x[0-9a-fA-F][0-9a-fA-F]) ;;
                            *) fail 'Invalid byte returned by i2ctransfer' ;;
                        esac
                        if [ "$((value & mask))" -ne "$((expected & mask))" ]; then
                            matched=0
                            break
                        fi
                    done
                    [ "$matched" -eq 1 ] || continue
                    printf 'Identified %s at %s on i2c-%s\n' "$type" "$address" "$bus"
                    [ -d "$sysfs/bus/i2c/drivers/$driver" ] || load_module "$module"
                    if ! printf '%s %s\n' "$type" "$address" > "$adapter/new_device"; then
                        [ -d "$client" ] || fail 'Could not instantiate identified bridge'
                    fi
                    [ "$(cat "$client/name")" = "$type" ] || fail 'Address claimed by another client'
                    wait_for_binding
                done
                IFS=$saved_ifs
                ;;
            *) fail 'Unexpected validated configuration record' ;;
        esac
    done <<EOF_CONFIG
$records
EOF_CONFIG
done
fail "No configured bridge identified on i2c-$bus"
