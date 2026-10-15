#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Boot the supplied base initramfs with a driver-only overlay and USB port passthrough."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--kernel", default=os.uname().release)
parser.add_argument(
    "--qemu",
    default="qemu-system-x86_64",
    help="QEMU executable, including a local patched build",
)
parser.add_argument(
    "--cpu",
    default="host",
    help="Guest CPU model; host exposes SIMD capabilities under KVM",
)
parser.add_argument(
    "--bios-path", type=Path, help="Firmware directory for a relocated QEMU build"
)
parser.add_argument("--modules", type=Path, default=ROOT)
parser.add_argument("--base", type=Path, default=ROOT / "base-initramfs.cpio.gz")
parser.add_argument("--hostbus", type=int, default=2)
parser.add_argument("--hostport", default="2")
parser.add_argument(
    "--kms-test", type=Path, help="Optional statically linked guest KMS test"
)
parser.add_argument("--output", type=Path, default=Path("/tmp/fl2000-qemu"))
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
run = Path(tempfile.mkdtemp(prefix="hardware-", dir=args.output))
entries = [
    (name, (args.modules / name).read_bytes(), 0o100644)
    for name in ("fl2000.ko", "it66121.ko")
]
entries += [
    ("fl2000-test-guest", b"FL2000 QEMU test guest\n", 0o100644),
    ("fl2000-startup", (ROOT / "scripts/startup.sh").read_bytes(), 0o100755),
]
for name in ("fl2000.ko", "it66121.ko"):
    version = subprocess.check_output(
        ["modinfo", "-F", "vermagic", str(args.modules / name)], text=True
    )
    if version.split()[0] != args.kernel:
        parser.error(f"{name} does not match kernel {args.kernel}")
if args.kms_test:
    entries.append(("kms-smoke", args.kms_test.read_bytes(), 0o100755))
entries.append(("TRAILER!!!", b"", 0))
archive = bytearray()
for ino, (name, data, mode) in enumerate(entries, 1):
    fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name) + 1, 0]
    archive.extend(b"070701" + "".join(f"{field:08x}" for field in fields).encode())
    archive.extend(name.encode() + b"\0")
    archive.extend(b"\0" * (-len(archive) % 4))
    archive.extend(data)
    archive.extend(b"\0" * (-len(archive) % 4))
archive.extend(b"\0" * (-len(archive) % 512))
(run / "payload.cpio").write_bytes(archive)
with (run / "boot.cpio").open("wb") as output:
    with args.base.open("rb") as base:
        shutil.copyfileobj(base, output)
    output.write(b"\0" * (-output.tell() % 4))
    output.write(archive)
print(f"Guest log: {run / 'guest.log'}", flush=True)
print("Inside the guest: /fl2000-startup", flush=True)
if args.kms_test:
    print("Then: mount -t debugfs none /sys/kernel/debug; /kms-smoke", flush=True)
command = [
    args.qemu,
    "-enable-kvm",
    "-cpu",
    args.cpu,
    "-m",
    "1G",
    "-nic",
    "none",
    "-kernel",
    f"/boot/vmlinuz-{args.kernel}",
    "-initrd",
    str(run / "boot.cpio"),
    "-append",
    "console=ttyS0 quiet panic=1 oops=panic",
    "-device",
    "qemu-xhci,id=xhci",
    "-device",
    f"usb-host,id=fl2000,bus=xhci.0,hostbus={args.hostbus},hostport={args.hostport}",
    "-qmp",
    f"unix:{run / 'qmp.sock'},server=on,wait=off",
    "-chardev",
    f"stdio,id=console,mux=on,logfile={run / 'guest.log'}",
    "-serial",
    "chardev:console",
    "-mon",
    "chardev=console,mode=readline",
    "-display",
    "none",
]
if args.bios_path:
    if not args.bios_path.is_dir():
        parser.error(f"Firmware directory does not exist: {args.bios_path}")
    command[1:1] = ["-L", str(args.bios_path)]
subprocess.run(command, check=True)
