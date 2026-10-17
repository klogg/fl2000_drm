#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Load only the test-guest modules; the prepared base supplies DRM dependencies.
set -eu

if [ "${1:-}" = "--help" ]; then
    printf '%s\n' 'Usage: startup.sh [module-directory]' \
        'Run inside the QEMU test guest. On the host, use scripts/emulate.sh.' \
        'Bridge candidates come from /etc/fl2000/enumerator.conf.'
    exit 0
fi
if [ "$#" -gt 1 ]; then
    printf '%s\n' 'Usage: startup.sh [module-directory]' >&2
    exit 2
fi
if [ ! -f /fl2000-test-guest ]; then
    printf '%s\n' 'Refusing module loading outside the prepared test guest; use scripts/emulate.sh.' >&2
    exit 1
fi
repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
module_dir=${1:-$repo_dir}
config=/etc/fl2000/enumerator.conf
if [ -x /fl2000-load-deps ]; then
    /fl2000-load-deps
fi
# The minimal guest has no installed module database. Preload supplied test
# modules from configuration; registered (including built-in) drivers need none.
awk '$1 == "bridge" { print $3, $4 }' "$config" |
while read -r module driver; do
    module_name=$(printf '%s' "$module" | tr '-' '_')
    if [ ! -d "/sys/bus/i2c/drivers/$driver" ] &&
       [ ! -d "/sys/module/$module_name" ]; then
        insmod "$module_dir/$module.ko"
    fi
done
if [ ! -d /sys/module/fl2000 ]; then
    insmod "$module_dir/fl2000.ko"
fi
adapter_name=$(awk '$1 == "adapter_name" { sub(/^[^ ]+ +/, ""); print }' "$config")
found=0
for adapter in /sys/bus/i2c/devices/i2c-*; do
    [ -r "$adapter/name" ] || continue
    [ "$(cat "$adapter/name")" = "$adapter_name" ] || continue
    /fl2000-enumerate "$adapter" "$config"
    found=1
done
[ "$found" -eq 1 ] || { echo 'No FL2000 I2C adapter found' >&2; exit 1; }
printf '%s\n' 'FL2000 transport and identified bridge loaded in the test guest.'
