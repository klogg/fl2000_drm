#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Append a newc overlay to the compressed base image and boot with KVM."""
from pathlib import Path
import argparse
import re
import subprocess

parser = argparse.ArgumentParser(description="KVM module load/unload smoke test without hardware")
parser.add_argument("kernel", help="Kernel version, e.g. 7.0.0-38-generic")
parser.add_argument("modules", type=Path, help="Directory containing matching built modules")
parser.add_argument("--base", type=Path, default=Path("base-initramfs.cpio.gz"))
parser.add_argument("--output", type=Path, default=Path("/tmp/fl2000-qemu"))
args = parser.parse_args()
kernel = args.kernel
source = args.modules
init=r'''#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/bin:/sbin:/usr/lib/klibc/bin
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
fail() { echo "FL2000_QEMU_FAIL: $*"; dmesg; /usr/lib/klibc/bin/poweroff -f; while :; do :; done; }
echo "FL2000_QEMU_KERNEL: $(uname -r)"
i=1
while [ "$i" -le 100 ]; do
 if [ $((i % 2)) -eq 0 ]; then
  insmod /fl2000.ko || fail "cycle $i USB load"
  insmod /it66121.ko || fail "cycle $i bridge load"
 else
  insmod /it66121.ko || fail "cycle $i bridge load"
  insmod /fl2000.ko || fail "cycle $i USB load"
 fi
 [ -d /sys/module/fl2000 ] || fail "USB module missing"
 [ -d /sys/module/it66121 ] || fail "bridge module missing"
 [ -d /sys/bus/usb/drivers/fl2000_usb ] || fail "USB driver missing"
 [ -d /sys/bus/i2c/drivers/it66121 ] || fail "I2C driver missing"
 if [ $((i % 2)) -eq 0 ]; then
  rmmod it66121 || fail "bridge unload"
  rmmod fl2000 || fail "USB unload"
 else
  rmmod fl2000 || fail "USB unload"
  rmmod it66121 || fail "bridge unload"
 fi
 [ ! -d /sys/module/fl2000 ] || fail "USB module remains"
 [ ! -d /sys/module/it66121 ] || fail "bridge module remains"
 [ ! -d /sys/bus/usb/drivers/fl2000_usb ] || fail "USB driver remains"
 [ ! -d /sys/bus/i2c/drivers/it66121 ] || fail "I2C driver remains"
 i=$((i + 1))
done
echo "FL2000_QEMU_PASS: 100 load/unload cycles, alternating load/unload order"
echo "FL2000_QEMU_TAINT: $(cat /proc/sys/kernel/tainted)"
echo "FL2000_QEMU_MODULES_BEGIN"
cat /proc/modules
echo "FL2000_QEMU_MODULES_END"
dmesg
echo "FL2000_QEMU_DONE"
/usr/lib/klibc/bin/poweroff -f
while :; do :; done
'''
archive=bytearray()
def add(name,data,mode,ino):
 fields=[ino,mode,0,0,1,0,len(data),0,0,0,0,len(name)+1,0]
 archive.extend(b'070701'+''.join(f'{x:08x}' for x in fields).encode())
 archive.extend(name.encode()+b'\0');archive.extend(b'\0'*(-len(archive)%4))
 archive.extend(data);archive.extend(b'\0'*(-len(archive)%4))
add('init',init.encode(),0o100755,1)
for i,name in enumerate(('fl2000.ko','it66121.ko'),2):add(name,(source/name).read_bytes(),0o100644,i)
add('TRAILER!!!',b'',0,4)
archive.extend(b'\0'*(-len(archive)%512))
p = args.output
p.mkdir(parents=True, exist_ok=True)
(p/f'payload-{kernel}.cpio').write_bytes(archive)
with (p/f'boot-{kernel}.cpio').open('wb') as f:
 with args.base.open('rb') as base:
  import shutil
  shutil.copyfileobj(base,f)
 f.write(b"\0" * (-f.tell() % 4))
 f.write(archive)

log = p / f"{kernel}.log"
command = [
    "qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-m", "512", "-smp", "2",
    "-nodefaults", "-no-reboot", "-display", "none", "-monitor", "none",
    "-serial", "stdio", "-nic", "none", "-kernel", f"/boot/vmlinuz-{kernel}",
    "-initrd", str(p / f"boot-{kernel}.cpio"),
    "-append", "console=ttyS0 rdinit=/init panic=1 oops=panic",
]
with log.open("w") as output:
    subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, timeout=60, check=True)
text = log.read_text()
assert f"FL2000_QEMU_KERNEL: {kernel}" in text, "Wrong guest kernel"
assert "FL2000_QEMU_PASS:" in text and "FL2000_QEMU_DONE" in text, f"Guest test incomplete: {log}"
assert not re.search(r"FL2000_QEMU_FAIL|WARNING:|BUG:|Oops:|Kernel panic|Unknown symbol|unpacking failed", text), f"Guest error: {log}"
assert "FL2000_QEMU_MODULES_BEGIN\nFL2000_QEMU_MODULES_END" in text, "Modules remain loaded"
assert "FL2000_QEMU_TAINT: 12288" in text, "Unexpected kernel taint"
print(f"PASS: {kernel}: 100 load/unload cycles; log: {log}")
