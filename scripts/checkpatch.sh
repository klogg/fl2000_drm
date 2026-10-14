#!/bin/bash
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source "$repo_dir/scripts/kernel-tools.sh"
if [[ ${1:-} == --help ]]; then
    printf '%s\n' 'Usage: checkpatch.sh [checkpatch-options]' \
        'Checks production C/headers in the repository, bridge and frame folders.' \
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
for file in *.[ch] bridge/*.[ch] frame/*.[ch]; do
    [[ $file == *.mod.c ]] && continue
    if ! perl "$checker" --strict --no-summary --no-tree "$@" -f "$file"; then
        status=1
    fi
done
exit "$status"
