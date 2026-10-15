#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
set -eu
repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
if [[ ${1:-} == --help ]]; then
    printf '%s\n' 'Usage: clang-format.sh [clang-format-options]' \
        'Formats production sources in place. --dry-run or -n checks without editing.' \
        'Override: CLANG_FORMAT (default: clang-format-18).'
    exit 0
fi
formatter=${CLANG_FORMAT:-clang-format-18}
command -v "$formatter" >/dev/null || { printf 'Missing formatter: %s\n' "$formatter" >&2; exit 1; }
if [[ ! -f "$repo_dir/.clang-format" ]]; then
    printf 'Missing repository style: %s\n' "$repo_dir/.clang-format" >&2
    exit 1
fi
mode=(-i)
for option in "$@"; do
    if [[ $option == --dry-run || $option == -n ]]; then
        mode=(--Werror)
    fi
done
cd "$repo_dir"
shopt -s nullglob
status=0
for file in *.[ch] bridge/*.[ch] frame/*.[ch]; do
    [[ $file == *.mod.c ]] && continue
    if ! "$formatter" "${mode[@]}" "$@" "$file"; then
        status=1
    fi
done
exit "$status"
