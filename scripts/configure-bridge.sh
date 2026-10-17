#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Instantiate an explicitly selected I2C bridge on its FL2000 adapter.
set -eu
if [ "${1:-}" = --help ]; then
    printf '%s\n' 'Usage: configure-bridge.sh TYPE HEX-ADDRESS [I2C-BUS-NUMBER]' \
        'Select the attached hardware explicitly; no chip detection is performed.'
    exit 0
fi
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    printf '%s\n' 'Usage: configure-bridge.sh TYPE HEX-ADDRESS [I2C-BUS-NUMBER]' >&2
    exit 2
fi
bridge_type=$1
address=$2
case "$bridge_type" in
    ''|*[!a-zA-Z0-9_-]*) printf '%s\n' 'Invalid I2C device type' >&2; exit 2 ;;
esac
case "$address" in
    0x*) digits=${address#0x} ;;
    *) printf '%s\n' 'Address must use hexadecimal 0x notation' >&2; exit 2 ;;
esac
case "$digits" in
    ''|*[!0-9a-fA-F]*) printf '%s\n' 'Invalid I2C address' >&2; exit 2 ;;
esac
address_number=$((address))
if [ "$address_number" -lt 3 ] || [ "$address_number" -gt 119 ]; then
    printf '%s\n' 'Address must be in the seven-bit I2C device range 0x03..0x77' >&2
    exit 2
fi
sysfs=${SYSFS_ROOT:-/sys}
selected=
for adapter in "$sysfs"/bus/i2c/devices/i2c-*; do
    [ -r "$adapter/name" ] || continue
    [ "$(cat "$adapter/name")" = 'FL2000 bridge I2C bus' ] || continue
    bus=${adapter##*/i2c-}
    [ "$#" -ne 3 ] || [ "$bus" = "$3" ] || continue
    if [ -n "$selected" ]; then
        printf '%s\n' 'Multiple FL2000 adapters; specify the I2C bus number' >&2
        exit 1
    fi
    selected=$adapter
done
if [ -z "$selected" ]; then
    printf '%s\n' 'No matching FL2000 I2C adapter' >&2
    exit 1
fi
bus=${selected##*/i2c-}
client="$sysfs/bus/i2c/devices/$bus-$(printf '%04x' "$address_number")"
if [ -e "$client" ]; then
    if [ "$(cat "$client/name")" != "$bridge_type" ]; then
        printf '%s\n' 'Another I2C device already occupies the requested address' >&2
        exit 1
    fi
else
    printf '%s %s\n' "$bridge_type" "$address" > "$selected/new_device"
fi
printf 'Configured %s at %s on I2C bus %s\n' "$bridge_type" "$address" "$bus"
