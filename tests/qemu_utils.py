# SPDX-License-Identifier: GPL-2.0
"""Shared configuration and newc overlays for isolated KVM tests."""

import os
import shutil
import subprocess
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / ".qemu-build/fl2000-tests"
LOCAL_QEMU = ROOT / ".qemu-build/qemu-11.1.2/build/qemu-system-x86_64"


def add_qemu_arguments(parser, *, kernel=True):
    if kernel:
        parser.add_argument(
            "--kernel", default=os.environ.get("KVER", os.uname().release)
        )
    parser.add_argument(
        "--qemu",
        default=os.environ.get("QEMU")
        or (str(LOCAL_QEMU) if LOCAL_QEMU.is_file() else "qemu-system-x86_64"),
    )
    parser.add_argument("--cpu", default="host")
    parser.add_argument(
        "--bios-path",
        type=Path,
        help="Firmware directory; inferred for a local source build",
    )
    parser.add_argument(
        "--base",
        type=Path,
        default=Path(os.environ.get("BASE_INITRAMFS", ROOT / "base-initramfs.cpio.gz")),
    )


def qemu_command(args, *, memory="1G"):
    binary = shutil.which(args.qemu)
    if not binary:
        raise FileNotFoundError(f"QEMU executable not found: {args.qemu}")
    command = [binary, "-enable-kvm", "-cpu", args.cpu, "-m", memory, "-nic", "none"]
    bios = args.bios_path
    if bios is None:
        candidate = Path(binary).resolve().parent.parent / "pc-bios"
        if candidate.is_dir():
            bios = candidate
    if bios is not None:
        if not bios.is_dir():
            raise FileNotFoundError(f"Firmware directory does not exist: {bios}")
        command += ["-L", str(bios.resolve())]
    return command


def kernel_headers(kernel, override=None):
    path = Path(override or f"/lib/modules/{kernel}/build")
    if not (path / "Makefile").is_file():
        raise FileNotFoundError(f"Kernel headers not found: {path}")
    return path.resolve()


def check_modules(paths, kernel):
    for path in paths:
        version = subprocess.check_output(
            ["modinfo", "-F", "vermagic", str(path)], text=True
        )
        if version.split()[0] != kernel:
            raise ValueError(f"{path} does not match kernel {kernel}")


def cpio_archive(entries):
    """Pack root-relative entries; callers supply any required directories."""
    archive = bytearray()
    entries = [*entries, ("TRAILER!!!", b"", 0)]
    for ino, (name, data, mode) in enumerate(entries, 1):
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts:
            raise ValueError(f"Unsafe archive path: {name}")
        name = name.encode() + b"\0"
        fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name), 0]
        archive += b"070701" + "".join(f"{field:08x}" for field in fields).encode()
        archive += name
        archive += b"\0" * (-len(archive) % 4)
        archive += data
        archive += b"\0" * (-len(archive) % 4)
    archive += b"\0" * (-len(archive) % 512)
    return archive


def write_initramfs(base, entries, boot, *, payload=None):
    """Append an aligned overlay without decompressing the supplied base."""
    archive = cpio_archive(entries)
    if payload is not None:
        payload.write_bytes(archive)
    with base.open("rb") as source, boot.open("wb") as output:
        shutil.copyfileobj(source, output)
        output.write(b"\0" * (-output.tell() % 4))
        output.write(archive)
