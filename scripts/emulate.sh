#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Append drivers to the prepared initramfs and pass a physical USB port to KVM.
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
qemu_build=$repo_dir/.qemu-build/qemu-11.1.2
qemu=${QEMU:-qemu-system-x86_64}
if [ -z "${QEMU:-}" ] && [ -x "$qemu_build/build/qemu-system-x86_64" ]; then
    qemu=$qemu_build/build/qemu-system-x86_64
    # The local build may have moved since configure; avoid its old BIOS path.
    set -- --bios-path "$qemu_build/pc-bios" "$@"
fi
exec python3 "$repo_dir/tests/qemu_hardware.py" \
    --kernel "${KVER:-$(uname -r)}" \
    --modules "${MODULE_DIR:-$repo_dir}" \
    --base "${BASE_INITRAMFS:-$repo_dir/base-initramfs.cpio.gz}" \
    --qemu "$qemu" \
    --output "${QEMU_OUTPUT_DIR:-$repo_dir/.qemu-build/fl2000-tests}" "$@"
