#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Run in Ubuntu 26.04 with packaging dependencies and Linux 7.0 headers.
set -euo pipefail
if [[ $# != 2 ]]; then
    echo "Usage: $0 SOURCE_DIRECTORY EMPTY_WORK_DIRECTORY" >&2
    exit 2
fi
source_dir=$(realpath "$1")
mkdir "$2"
work=$(realpath "$2")
trap 'status=$?; echo "Packaging checks failed at line $LINENO (status $status)" >&2; exit "$status"' ERR
version=$(cd "$source_dir" && dpkg-parsechangelog -S Version)
upstream=${version%-*}
ksrc=$(find /usr/src -maxdepth 1 -type d -name 'linux-headers-7.0.*-generic' | sort -V | tail -n 1)
test -n "$ksrc"
kernel=${ksrc##*/linux-headers-}
cd "$source_dir"
shellcheck tests/check_packages.sh tests/packaging_ci.sh debian/tests/dkms debian/tests/enumerator
bash -n debian/fl2000-dkms.dkms
shellcheck --shell bash --exclude SC2034,SC2154 debian/fl2000-dkms.dkms
# Export tracked files, excluding ignored build products and local test artifacts.
mkdir "$work/source"
git archive HEAD | tar -x -C "$work/source"
git archive --format=tar --prefix="fl2000-drm-$upstream/" HEAD -- . ':!debian' |
    xz > "$work/fl2000-drm_$upstream.orig.tar.xz"
# Builders run without root even when invoked in a root-owned CI container.
builder=()
if [[ $(id -u) == 0 ]]; then
    chown -R nobody:nogroup "$work"
    builder=(runuser -u nobody --)
fi
cd "$work/source"
"${builder[@]}" dpkg-buildpackage -us -uc > "$work/build.log" 2>&1
"${builder[@]}" dpkg-buildpackage -S -sa -us -uc > "$work/source-build.log" 2>&1
cd "$work"
"${builder[@]}" lintian --fail-on error,warning \
    "fl2000-drm_${version}_amd64.changes" \
    "fl2000-drm_${version}_source.changes" > lintian.log 2>&1
mkdir roundtrip
if [[ $(id -u) == 0 ]]; then chown nobody:nogroup roundtrip; fi
"${builder[@]}" dpkg-source -x "fl2000-drm_${version}.dsc" roundtrip/source > rebuild.log 2>&1
(cd roundtrip/source && "${builder[@]}" dpkg-buildpackage -b -us -uc) >> rebuild.log 2>&1
for package in fl2000-dkms fl2000-enumerator; do
    cmp "${package}_${version}_all.deb" "roundtrip/${package}_${version}_all.deb"
done
"$source_dir/tests/check_packages.sh" "$work/source" "$work" | tee contents.log
mkdir "$work/extracted" "$work/dkms" "$work/install"
dpkg-deb -x "fl2000-dkms_${version}_all.deb" "$work/extracted"
dkms_args=(--sourcetree "$work/extracted/usr/src" --dkmstree "$work/dkms" --installtree "$work/install")
dkms add -m fl2000-drm -v "$upstream" "${dkms_args[@]}" > dkms.log 2>&1
dkms build -m fl2000-drm -v "$upstream" -k "$kernel" \
    --kernelsourcedir "$ksrc" "${dkms_args[@]}" >> dkms.log 2>&1
for module in fl2000 it66121; do
    path="$work/dkms/fl2000-drm/$upstream/$kernel/x86_64/module/$module.ko"
    [[ $(modinfo -F vermagic "$path") == "$kernel "* ]]
done
# Unsupported targets must be skipped rather than built accidentally.
for target in kernel architecture; do
    args=(-k "$kernel" -a x86_64)
    if [[ $target == kernel ]]; then args=(-k 7.2.0-fl2000-test -a x86_64); else args=(-k "$kernel" -a aarch64); fi
    status=0
    dkms build -m fl2000-drm -v "$upstream" "${args[@]}" \
        --kernelsourcedir "$ksrc" "${dkms_args[@]}" >> dkms.log 2>&1 || status=$?
    [[ $status == 77 ]]
done
printf 'Source and binary builds, Lintian, package contents and DKMS: PASS\n' | tee result.log
