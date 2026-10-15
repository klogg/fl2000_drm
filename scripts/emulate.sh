#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Append drivers to the prepared initramfs and pass a physical USB port to KVM.
set -eu
repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
exec python3 "$repo_dir/tests/qemu_hardware.py" \
    --kernel "${KVER:-$(uname -r)}" \
    --modules "${MODULE_DIR:-$repo_dir}" \
    --base "${BASE_INITRAMFS:-$repo_dir/base-initramfs.cpio.gz}" \
    --output "${QEMU_OUTPUT_DIR:-$repo_dir/.qemu-build/fl2000-tests}" "$@"
