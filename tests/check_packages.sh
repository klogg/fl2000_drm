#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Check binary package contents without installing packages or loading modules.
set -euo pipefail
if [[ $# != 2 ]]; then
    echo "Usage: $0 SOURCE_DIRECTORY ARTIFACT_DIRECTORY" >&2
    exit 2
fi
source_dir=$(realpath "$1")
artifacts=$(realpath "$2")
version=$(cd "$source_dir" && dpkg-parsechangelog -S Version)
upstream=${version%-*}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for package in fl2000-dkms fl2000-enumerator; do
    archive="$artifacts/${package}_${version}_all.deb"
    [[ $(dpkg-deb -f "$archive" Package) == "$package" ]]
    [[ $(dpkg-deb -f "$archive" Version) == "$version" ]]
    [[ $(dpkg-deb -f "$archive" Architecture) == all ]]
    dpkg-deb -x "$archive" "$work/root"
    dpkg-deb -e "$archive" "$work/$package"
    for script in preinst postinst prerm postrm; do
        if [[ -f "$work/$package/$script" ]]; then
            sh -n "$work/$package/$script"
        fi
    done
done
driver="$work/root/usr/src/fl2000-drm-$upstream"
cd "$source_dir"
for file in Makefile Kbuild LICENSE fl2000_*.c fl2000*.h bridge/*.[ch] frame/*.[ch] compat/*.[ch]; do
    cmp "$file" "$driver/$file"
done
for file in debian/vendor/linux-7.0/*.c; do
    cmp "$file" "$driver/compat/linux-7.0/${file##*/}"
done
sed "s/#MODULE_VERSION#/$upstream/g" debian/fl2000-dkms.dkms > "$work/dkms.conf"
cmp "$work/dkms.conf" "$driver/dkms.conf"
for pair in \
    'fl2000-enumerate.sh usr/libexec/fl2000-enumerate' \
    'fl2000-enumerate@.service usr/lib/systemd/system/fl2000-enumerate@.service' \
    '70-fl2000.rules usr/lib/udev/rules.d/70-fl2000.rules' \
    'enumerator.conf etc/fl2000/enumerator.conf'; do
    read -r original installed <<< "$pair"
    cmp "userspace/$original" "$work/root/$installed"
done
[[ $(stat -c %a "$work/root/usr/libexec/fl2000-enumerate") == 755 ]]
grep -Fx '/etc/fl2000/enumerator.conf' "$work/fl2000-enumerator/conffiles"
"$work/root/usr/libexec/fl2000-enumerate" --help
printf 'Package contents and maintainer script syntax: PASS\n'
