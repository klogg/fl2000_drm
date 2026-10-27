#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
set -eu
if [[ ${1:-} == --help ]]; then
    printf '%s\n' 'Usage: fetch-bridge-connector.sh' \
        'Prepares checksum-pinned Linux 7.0 DRM helpers from vendored sources or HTTPS.'
    exit 0
fi
repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
output="$repo_dir/.qemu-build/upstream"
mkdir -p "$output"
temporary=$(mktemp "$output/connector.XXXXXX")
trap 'rm -f "$temporary" "$temporary.b64"' EXIT
while read -r name checksum; do
    vendor_source="$repo_dir/debian/vendor/linux-7.0/$name.c"
    if [[ -f "$vendor_source" ]]; then
        cp "$vendor_source" "$temporary"
    else
        curl --fail --silent --show-error --location \
            --retry 3 --retry-delay 2 --retry-max-time 60 \
            --connect-timeout 10 --max-time 30 \
            --proto '=https' --proto-redir '=https' \
            "https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/v7.0/drivers/gpu/drm/display/$name.c?format=TEXT" \
            --output "$temporary.b64"
        base64 --decode "$temporary.b64" > "$temporary"
    fi
    printf '%s  %s\n' "$checksum" "$temporary" | sha256sum --check --status
    mv "$temporary" "$output/$name.c"
done <<'SOURCES'
drm_bridge_connector d55a4e08317ff15c2ef970550007c47127902165be6611a817d5fd8e4eaf060e
drm_hdmi_state_helper dbd579de2280368a9eb0ba9201e0b58af01edee74027cd4488841748ca2b3fcb
drm_hdmi_audio_helper b980718b199c11a1db4c2bf4c8b060da3db63b5c1dca0ed1546721f22bf1ad49
drm_hdmi_cec_helper 9875ba27feabe83bdbbc8c6374703a3f83e68c6ef3ed97bf70fd18f7e031a902
SOURCES
