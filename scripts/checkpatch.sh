#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
set -eu
repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
source "$repo_dir/scripts/kernel-tools.sh"
if [[ ${1:-} == --help ]]; then
    printf '%s\n' 'Usage: checkpatch.sh [checkpatch-options]' \
        'Checks production C/headers in the repository, bridge, frame and compat folders.' \
        'Overrides: CHECKPATCH, KSRC, KVER, KERNEL_TOOLS_TAG.'
    exit 0
fi
checker=${CHECKPATCH:-$(kernel_tool scripts/checkpatch.pl)}
# Downloaded checkpatch uses companion resources beside the script.
if [[ $checker == "$repo_dir/.cache/"* ]]; then
    kernel_tool scripts/spelling.txt cache >/dev/null
    kernel_tool scripts/const_structs.checkpatch cache >/dev/null
fi
cd "$repo_dir"
shopt -s nullglob
status=0
for file in *.[ch] bridge/*.[ch] frame/*.[ch] compat/*.[ch]; do
    [[ $file == *.mod.c ]] && continue
    options=()
    case "$file" in
        frame/ssse3.c)
            # GCC SIMD builtins require these primitive vector type aliases.
            options+=(--ignore NEW_TYPEDEFS)
            ;;
        fl2000_drv.c|fl2000_interrupt.c)
            # USB descriptor field names are defined by the kernel ABI.
            options+=(--ignore CAMELCASE)
            ;;
        *)
            ;;
    esac
    if ! perl "$checker" --strict --no-summary --no-tree "${options[@]}" "$@" -f "$file"; then
        status=1
    fi
done
exit "$status"
