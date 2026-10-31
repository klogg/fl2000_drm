#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
set -eu
if [[ ${1:-} == --help ]]; then
    printf '%s\n' 'Usage: fetch-bridge-connector.sh [7.0|7.3]' \
        'Prepares checksum-pinned DRM helpers from vendored sources or HTTPS.' \
        'Defaults to Linux 7.0; select the target kernel API version explicitly.'
    exit 0
fi
repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
version=${1:-7.0}
case "$version" in
    7.0)
        output="$repo_dir/.qemu-build/upstream"
        connector_checksum=d55a4e08317ff15c2ef970550007c47127902165be6611a817d5fd8e4eaf060e
        state_checksum=dbd579de2280368a9eb0ba9201e0b58af01edee74027cd4488841748ca2b3fcb
        ;;
    7.3)
        output="$repo_dir/.qemu-build/upstream-$version"
        connector_checksum=ba529f21010d378e80046358fd11ec958c483aeb07c6a91fb7c4c71e8f7eec81
        state_checksum=a7fa4520de8373d1d621d9300ba9b5084088fa0e61dbfa2bb95f0f5395dde52c
        ;;
    *)
        printf 'Unsupported helper version: %s; supply matching sources with FL2000_HELPER_SOURCE_DIR.\n' "$version" >&2
        exit 1
        ;;
esac
if [[ $# -gt 1 ]]; then
    printf '%s\n' 'Expected at most one kernel API version.' >&2
    exit 1
fi
mkdir -p "$output"
temporary=$(mktemp "$output/connector.XXXXXX")
trap 'rm -f "$temporary" "$temporary.b64"' EXIT
while read -r name checksum; do
    vendor_source="$repo_dir/debian/vendor/linux-$version/$name.c"
    if [[ -f "$vendor_source" ]]; then
        cp "$vendor_source" "$temporary"
    else
        curl --fail --silent --show-error --location \
            --retry 3 --retry-delay 2 --retry-max-time 60 \
            --connect-timeout 10 --max-time 30 \
            --proto '=https' --proto-redir '=https' \
            "https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/v$version/drivers/gpu/drm/display/$name.c?format=TEXT" \
            --output "$temporary.b64"
        base64 --decode "$temporary.b64" > "$temporary"
    fi
    printf '%s  %s\n' "$checksum" "$temporary" | sha256sum --check --status
    mv "$temporary" "$output/$name.c"
done <<SOURCES
drm_bridge_connector $connector_checksum
drm_hdmi_state_helper $state_checksum
drm_hdmi_audio_helper b980718b199c11a1db4c2bf4c8b060da3db63b5c1dca0ed1546721f22bf1ad49
drm_hdmi_cec_helper 9875ba27feabe83bdbbc8c6374703a3f83e68c6ef3ed97bf70fd18f7e031a902
SOURCES
