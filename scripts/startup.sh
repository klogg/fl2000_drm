#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Load only the test-guest modules; the prepared base supplies DRM dependencies.
set -eu

if [ "${1:-}" = "--help" ]; then
    printf '%s\n' 'Usage: startup.sh [module-directory]' \
        'Run inside the QEMU test guest. On the host, use scripts/emulate.sh.' \
        'Test fixture overrides: BRIDGE_MODULE, BRIDGE_TYPE, BRIDGE_ADDR, I2C_BUS.'
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
bridge_module=${BRIDGE_MODULE:-it66121}
bridge_type=${BRIDGE_TYPE:-it66121}
bridge_addr=${BRIDGE_ADDR:-0x4c}
for module in "$bridge_module" fl2000; do
    if [ ! -r "$module_dir/$module.ko" ]; then
        printf 'Missing module: %s\n' "$module_dir/$module.ko" >&2
        exit 1
    fi
done
if [ -x /fl2000-load-deps ]; then
    /fl2000-load-deps
fi
loaded_bridge=0
if [ ! -d "/sys/module/$bridge_module" ]; then
    insmod "$module_dir/$bridge_module.ko"
    loaded_bridge=1
fi
if [ ! -d /sys/module/fl2000 ] && ! insmod "$module_dir/fl2000.ko"; then
    if [ "$loaded_bridge" -eq 1 ]; then
        rmmod "$bridge_module" || true
    fi
    exit 1
fi
/fl2000-configure-bridge "$bridge_type" "$bridge_addr" ${I2C_BUS:+"$I2C_BUS"}
printf '%s\n' 'FL2000 transport and configured bridge loaded in the test guest.'
