#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Shared by the style scripts. Prefer installed kernel resources, then a pinned tag.
kernel_tool() {
    local resource=$1 source_policy=${2:-installed} kernel_dir cache_dir temporary
    kernel_dir=${KSRC:-/lib/modules/${KVER:-$(uname -r)}/build}
    if [[ $source_policy != cache && -r "$kernel_dir/$resource" ]]; then
        printf '%s\n' "$kernel_dir/$resource"
        return
    fi
    local tag=${KERNEL_TOOLS_TAG:-v7.0}
    if [[ ! $tag =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]]; then
        printf 'Invalid KERNEL_TOOLS_TAG: %s\n' "$tag" >&2
        return 1
    fi
    cache_dir=${repo_dir:?caller must set repo_dir}/.cache/kernel-tools/$tag
    mkdir -p "$cache_dir/$(dirname -- "$resource")" || return
    if [[ ! -s "$cache_dir/$resource" ]]; then
        temporary=$(mktemp "$cache_dir/$resource.XXXXXX") || return
        local -a curl_options=(
            --fail --location --proto '=https' --proto-redir '=https'
            --silent --show-error --retry 3 --retry-delay 2 --retry-max-time 30
            --connect-timeout 10 --max-time 20
        )
        if ! curl "${curl_options[@]}" \
            "https://raw.githubusercontent.com/torvalds/linux/$tag/$resource" -o "$temporary"; then
            if ! curl "${curl_options[@]}" \
                "https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/$tag/$resource?format=TEXT" \
                -o "$temporary.b64" || ! base64 --decode "$temporary.b64" > "$temporary"; then
                rm -f -- "$temporary" "$temporary.b64"
                return 1
            fi
            rm -f -- "$temporary.b64"
        fi
        mv -- "$temporary" "$cache_dir/$resource" || return
    fi
    printf '%s\n' "$cache_dir/$resource"
}
