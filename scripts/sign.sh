#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
set -eu
if [ "${1:-}" = "--help" ]; then
    printf '%s\n' 'Usage: sign.sh' \
        'Overrides: KVER, KSRC, MODULE_DIR, KEY_DIR, SIGN_KEY, SIGN_CERT, SIGN_CMD.'
    exit 0
fi
if [ "$#" -ne 0 ]; then
    printf '%s\n' 'Usage: sign.sh (configure paths through environment variables)' >&2
    exit 2
fi
repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
module_dir=${MODULE_DIR:-$repo_dir}
kernel_dir=${KSRC:-/lib/modules/${KVER:-$(uname -r)}/build}
sign_file=${SIGN_CMD:-$kernel_dir/scripts/sign-file}
key_dir=${KEY_DIR:-/var/lib/shim-signed/mok}
key_file=${SIGN_KEY:-$key_dir/MOK.priv}
cert_file=${SIGN_CERT:-$key_dir/MOK.der}
if [ ! -x "$sign_file" ]; then
    printf 'Signing tool is not executable: %s\n' "$sign_file" >&2
    exit 1
fi
for file in "$key_file" "$cert_file" "$module_dir/fl2000.ko" "$module_dir/it66121.ko"; do
    if [ ! -r "$file" ]; then
        printf 'Signing input is not readable: %s\n' "$file" >&2
        exit 1
    fi
done
for module in fl2000 it66121; do
    "$sign_file" sha256 "$key_file" "$cert_file" "$module_dir/$module.ko"
    printf 'Signed %s\n' "$module_dir/$module.ko"
done
