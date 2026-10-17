#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Boot the supplied base initramfs with a driver-only overlay and USB port passthrough."""

import argparse
import subprocess
import tempfile
from pathlib import Path

from qemu_utils import (
    OUTPUT,
    ROOT,
    add_qemu_arguments,
    check_modules,
    dependency_overlay,
    qemu_command,
    write_initramfs,
)

parser = argparse.ArgumentParser(description=__doc__)
add_qemu_arguments(parser)
parser.add_argument("--modules", type=Path, default=ROOT)
parser.add_argument("--hostbus", type=int, default=2)
parser.add_argument("--hostport", default="2")
parser.add_argument(
    "--kms-test", type=Path, help="Optional statically linked guest KMS test"
)
parser.add_argument("--output", type=Path, default=OUTPUT)
parser.add_argument(
    "--no-device",
    action="store_true",
    help="Attach the USB port later from the QEMU monitor",
)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
run = Path(tempfile.mkdtemp(prefix="hardware-", dir=args.output))
entries = [
    (name, (args.modules / name).read_bytes(), 0o100644)
    for name in ("fl2000.ko", "it66121.ko")
]
entries += [
    (
        "fl2000-configure-bridge",
        (ROOT / "scripts/configure-bridge.sh").read_bytes(),
        0o100755,
    ),
    ("fl2000-test-guest", b"FL2000 QEMU test guest\n", 0o100644),
    ("fl2000-startup", (ROOT / "scripts/startup.sh").read_bytes(), 0o100755),
]
check_modules(
    [args.modules / name for name in ("fl2000.ko", "it66121.ko")], args.kernel
)
if args.kms_test:
    entries.append(("kms-smoke", args.kms_test.read_bytes(), 0o100755))
entries += dependency_overlay(
    [args.modules / "fl2000.ko", args.modules / "it66121.ko"], args.kernel
)
write_initramfs(args.base, entries, run / "boot.cpio", payload=run / "payload.cpio")
print(f"Guest log: {run / 'guest.log'}", flush=True)
print("Inside the guest: /fl2000-startup", flush=True)
if args.kms_test:
    print("Then: mount -t debugfs none /sys/kernel/debug; /kms-smoke", flush=True)
command = qemu_command(args) + [
    "-kernel",
    f"/boot/vmlinuz-{args.kernel}",
    "-initrd",
    str(run / "boot.cpio"),
    "-append",
    "console=ttyS0 quiet panic=1 oops=panic",
    "-device",
    "qemu-xhci,id=xhci",
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
if not args.no_device:
    command += [
        "-device",
        f"usb-host,id=fl2000,bus=xhci.0,hostbus={args.hostbus},hostport={args.hostport}",
    ]
else:
    print("Attach later in the QEMU monitor (Ctrl-a c):", flush=True)
    print(
        f"device_add usb-host,id=fl2000,bus=xhci.0,hostbus={args.hostbus},hostport={args.hostport}",
        flush=True,
    )
subprocess.run(command, check=True)
