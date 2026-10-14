#!/bin/sh
# Load only the test-guest modules; the prepared base supplies DRM dependencies.
set -eu

if [ "${1:-}" = "--help" ]; then
    printf '%s\n' 'Usage: startup.sh [module-directory]' \
        'Run inside the QEMU test guest. On the host, use scripts/emulate.sh.'
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
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
module_dir=${1:-$repo_dir}
for module in it66121 fl2000; do
    if [ ! -r "$module_dir/$module.ko" ]; then
        printf 'Missing module: %s\n' "$module_dir/$module.ko" >&2
        exit 1
    fi
done
loaded_bridge=0
if [ ! -d /sys/module/it66121 ]; then
    insmod "$module_dir/it66121.ko"
    loaded_bridge=1
fi
if [ ! -d /sys/module/fl2000 ]; then
    if ! insmod "$module_dir/fl2000.ko"; then
        if [ "$loaded_bridge" -eq 1 ]; then
            rmmod it66121 || true
        fi
        exit 1
    fi
fi
printf '%s\n' 'FL2000 and IT66121 loaded in the test guest.'
