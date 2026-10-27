#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

if [[ ${1:-} == --help ]]; then
    printf 'Usage: %s [sparse|smatch]\nBuild upstream HEAD; no argument builds both analyzers.\n' "$0"
    printf 'Overrides: STATIC_TOOLS_DIR, ANALYSIS_LOG_DIR, SPARSE_REF, SMATCH_REF, JOBS, CFLAGS\n'
    exit 0
fi
if [[ $# -gt 1 || ${1:-all} != @(all|sparse|smatch) ]]; then
    printf 'Usage: %s [sparse|smatch]\n' "$0" >&2
    exit 1
fi

repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
tools_dir=${STATIC_TOOLS_DIR:-$repo_dir/.cache/analyzers}
log_dir=${ANALYSIS_LOG_DIR:-$repo_dir/.qemu-build/static-analysis}
mkdir -p "$tools_dir" "$log_dir"
tools_dir=$(realpath -- "$tools_dir")
log_dir=$(realpath -- "$log_dir")

while read -r tool repository; do
    [[ ${1:-all} == all || $1 == "$tool" ]] || continue
    case $tool in
        sparse) reference=${SPARSE_REF:-HEAD} ;;
        smatch) reference=${SMATCH_REF:-HEAD} ;;
    esac
    source_dir="$tools_dir/$tool"
    : > "$log_dir/source-$tool.log"
    (
        printf 'Fetching %s from %s at %s\n' "$tool" "$repository" "$reference"
        if [[ ! -d "$source_dir/.git" ]]; then
            git init --quiet "$source_dir"
        fi
        git -C "$source_dir" fetch --depth=1 -- "$repository" "$reference"
        revision=$(git -C "$source_dir" rev-parse --verify 'FETCH_HEAD^{commit}')
        git -C "$source_dir" checkout --quiet --detach "$revision"
        if [[ -n $(git -C "$source_dir" status --porcelain --untracked-files=no) ]]; then
            printf 'Analyzer sources contain local changes: %s\n' "$source_dir" >&2
            exit 1
        fi
        printf 'Tool: %s\nRepository: %s\nRef: %s\nCommit: %s\n' \
            "$tool" "$repository" "$reference" "$revision" | tee "$log_dir/source-$tool.log"
        git -C "$source_dir" log -1 --format='%H %cs %s'
        CFLAGS=${CFLAGS:--O2 -g} make -C "$source_dir" -j "${JOBS:-2}" "$tool"
        "$source_dir/$tool" --version
    ) 2>&1 | tee "$log_dir/build-$tool.log"
done <<'SOURCES'
sparse https://git.kernel.org/pub/scm/devel/sparse/sparse.git
smatch https://repo.or.cz/smatch.git
SOURCES
