#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source "$repo_dir/scripts/kernel-tools.sh"
if [[ ${1:-} == --help ]]; then
    printf '%s\n' 'Usage: clang-format.sh [clang-format-options]' \
        'Formats production sources in place. --dry-run or -n checks without editing.' \
        'Overrides: CLANG_FORMAT, KSRC, KVER, KERNEL_TOOLS_TAG.'
    exit 0
fi
formatter=${CLANG_FORMAT:-clang-format}
command -v "$formatter" >/dev/null || { printf 'Missing formatter: %s\n' "$formatter" >&2; exit 1; }
if [[ ! -f "$repo_dir/.clang-format" ]]; then
    style=$(kernel_tool .clang-format)
    python3 - "$style" "$repo_dir/.clang-format" <<'PY'
from pathlib import Path
import re
import sys
style = Path(sys.argv[1]).read_text()
for key, value in [('ColumnLimit', '100'), ('AlignConsecutiveMacros', 'true'),
                   ('AlignConsecutiveBitFields', 'true')]:
    pattern = rf'(?m)^{key}:.*$'
    if re.search(pattern, style):
        style = re.sub(pattern, f'{key}: {value}', style)
    else:
        style += f'\n{key}: {value}\n'
Path(sys.argv[2]).write_text(style)
PY
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
