#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

# Kbuild invokes this adapter with compiler flags followed by a source filename.
if [[ ${1:-} == --check ]]; then
    shift
    [[ $# -gt 0 ]] || exit 1
    case ${!#} in
        frame/ssse3.c|*/frame/ssse3.c|frame/bmi2.c|*/frame/bmi2.c)
            printf 'SKIP %s: checker lacks x86 vector/instruction builtins\n' "${!#}"
            exit 0
            ;;
    esac
    read -r -a checker_args <<< "${FL2000_CHECKER:?missing checker command}"
    exec "${checker_args[@]}" "$@"
fi

if [[ $# != 1 || $1 == --help ]]; then
    printf 'Usage: %s {sparse|smatch|coccinelle}\n' "$0"
    printf 'Overrides: KSRC, STATIC_TOOLS_DIR, SPARSE, SMATCH, SPATCH, ANALYSIS_LOG_DIR\n'
    [[ ${1:-} == --help ]] && exit 0
    exit 1
fi
repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel_dir=${KSRC:-/lib/modules/${KVER:-$(uname -r)}/build}
kernel_dir=$(realpath -- "$kernel_dir")
tools_dir=${STATIC_TOOLS_DIR:-$repo_dir/.cache/analyzers}
log_dir=${ANALYSIS_LOG_DIR:-$repo_dir/.qemu-build/static-analysis}
mkdir -p "$log_dir"
cd "$repo_dir"

case $1 in
    sparse|smatch)
        if [[ $1 == sparse ]]; then
            checker=${SPARSE:-$tools_dir/sparse/sparse}
        else
            checker="${SMATCH:-$tools_dir/smatch/smatch} -p=kernel --fatal-checks"
            checker+=" --data=$tools_dir/smatch/smatch_data"
        fi
        # Kbuild warns and disables checking when this test fails, yet builds on.
        read -r -a checker_args <<< "$checker"
        if [[ $("$kernel_dir/scripts/checker-valid.sh" "${checker_args[@]}") != 1 ]]; then
            printf 'Checker cannot parse the target kernel: %s\n' "$checker" >&2
            exit 1
        fi
        probe="$log_dir/$1-control.c"
        if [[ $1 == sparse ]]; then
            printf 'int *f(int __attribute__((address_space(1))) *p) { return p; }\n' > "$probe"
            expected='different address spaces'
        else
            printf 'int f(int *p) { if (!p) return *p; return 0; }\n' > "$probe"
            expected='could be null'
        fi
        if "${checker_args[@]}" -Wsparse-error "$probe" > "$log_dir/$1-control.log" 2>&1; then
            printf '%s did not reject the intentional defect\n' "$1" >&2
            exit 1
        fi
        rg -q "$expected" "$log_dir/$1-control.log"
        printf '%s failure control passed\n' "$1"
        printf 'Checker: %s\n' "$checker"
        export FL2000_CHECKER=$checker
        for backend in generic ssse3 bmi2; do
            log="$log_dir/$1-$backend.log"
            make -C "$kernel_dir" M="$repo_dir" FL2000_FRAME_BACKEND="$backend" \
                C=2 CHECK="$repo_dir/scripts/static-analysis.sh --check" \
                CF=-Wsparse-error W=1 KCFLAGS=-Werror modules \
                2>&1 | tee "$log"
            if ! rg -q '^  CHECK +fl2000_drv.c$' "$log"; then
                printf 'Kbuild did not run the checker; see %s\n' "$log" >&2
                exit 1
            fi
        done
        ;;
    coccinelle)
        spatch=${SPATCH:-spatch}
        "$spatch" --version
        probe="$log_dir/cocci-control.c"
        cat > "$probe" <<'CONTROL'
struct item { int value; };
int f(struct item *p) { if (p == NULL) { return p->value; } return 0; }
CONTROL
        "$spatch" --sp-file "$kernel_dir/scripts/coccinelle/null/deref_null.cocci" \
            -D report --no-show-diff --very-quiet "$probe" \
            > "$log_dir/cocci-control.log" 2>&1
        rg -q 'is NULL but dereferenced' "$log_dir/cocci-control.log"
        printf 'Coccinelle failure control passed\n'
        sources=(fl2000_*.c bridge/*.c frame/*.c compat/*.c)
        # Focus on error, lifetime and locking defects rather than rewrite rules.
        rules=(
            null/deref_null null/eno free/devm_free free/kfreeaddr
            locks/double_lock locks/flags tests/doubletest tests/doublebitand
            misc/irqf_oneshot misc/uninitialized_var
        )
        for rule in "${rules[@]}"; do
            log="$log_dir/cocci-${rule//\//-}.log"
            printf 'Coccinelle: %s\n' "$rule"
            "$spatch" --sp-file "$kernel_dir/scripts/coccinelle/$rule.cocci" \
                -D report --no-show-diff --very-quiet "${sources[@]}" \
                > "$log" 2>&1
            cat "$log"
            # Report mode can return success even when a semantic match is found.
            if [[ -s $log ]]; then
                printf 'Coccinelle diagnostics from %s; see %s\n' "$rule" "$log" >&2
                exit 1
            fi
        done
        ;;
    *) printf 'Unknown analyzer: %s\n' "$1" >&2; exit 1 ;;
esac
