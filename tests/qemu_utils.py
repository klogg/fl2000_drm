# SPDX-License-Identifier: GPL-2.0
"""Shared configuration and newc overlays for isolated KVM tests."""

import gzip
import lzma
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
        "--bulk-out-pipeline",
        action="store_true",
        help="Enable bulk-OUT queueing (requires the optional QEMU patch)",
    )
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
    if getattr(args, "bulk_out_pipeline", False):
        command += ["-global", "usb-host.bulk-out-pipeline=on"]
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


def dependency_overlay(modules, kernel):
    """Read installed dependency modules for guest-only loading, in load order."""
    entries = []
    commands = ["#!/bin/sh", "set -eu"]
    unload = []
    visited = set()

    def visit(module):
        dependencies = subprocess.check_output(
            ["modinfo", "-F", "depends", str(module)], text=True
        ).strip()
        for name in filter(None, dependencies.split(",")):
            name = name.replace("-", "_")
            if name in visited:
                continue
            visited.add(name)
            filename = subprocess.check_output(
                ["modinfo", "-k", kernel, "-F", "filename", name], text=True
            ).strip()
            if filename == "(builtin)":
                continue
            path = Path(filename)
            visit(path)
            data = path.read_bytes()
            if path.suffix == ".zst":
                try:
                    from compression import zstd
                except ImportError:
                    data = subprocess.check_output(
                        ["zstd", "--decompress", "--stdout", str(path)]
                    )
                else:
                    data = zstd.decompress(data)
            elif path.suffix == ".xz":
                data = lzma.decompress(data)
            elif path.suffix == ".gz":
                data = gzip.decompress(data)
            target = f"fl2000-dep-{name}.ko"
            entries.append((target, data, 0o100644))
            commands.append(
                f"if [ ! -d /sys/module/{name} ]; then\n"
                f" insmod /{target}\n : > /{target}.loaded\nfi"
            )
            unload.insert(
                0,
                f"if [ -f /{target}.loaded ]; then\n"
                f" rmmod {name}\n rm -f /{target}.loaded\nfi",
            )

    for module in modules:
        visit(module)
    entries.append(
        ("fl2000-load-deps", ("\n".join(commands) + "\n").encode(), 0o100755)
    )
    entries.append(
        (
            "fl2000-unload-deps",
            ("\n".join(["#!/bin/sh", "set -eu", *unload]) + "\n").encode(),
            0o100755,
        )
    )
    return entries


def enumeration_overlay(i2ctransfer):
    """Supply the production enumerator and its approved I2C utility to a guest."""
    executable = Path(i2ctransfer or shutil.which("i2ctransfer") or "")
    if not executable.is_file():
        raise FileNotFoundError("Install i2c-tools or supply --i2ctransfer PATH")
    files = {"usr/sbin/i2ctransfer": executable}
    # Include shared libraries without installing anything on the host or guest.
    for binary in (executable, Path(shutil.which("flock") or "")):
        if not binary.is_file():
            raise FileNotFoundError("util-linux flock is required")
        if binary != executable:
            files["usr/bin/flock"] = binary
        output = subprocess.check_output(["ldd", str(binary)], text=True)
        for line in output.splitlines():
            for token in line.split():
                if token.startswith("/"):
                    files[
                        "usr/" + token.lstrip("/")
                        if token.startswith(("/lib/", "/lib64/"))
                        else token.lstrip("/")
                    ] = Path(token)
    directories = {"etc", "etc/fl2000", "usr", "usr/bin", "usr/sbin"}
    for name in files:
        directories.update(
            str(parent) for parent in PurePosixPath(name).parents if str(parent) != "."
        )
    entries = [
        (name, b"", 0o40755)
        for name in sorted(directories, key=lambda name: (name.count("/"), name))
    ]
    entries += [(name, path.read_bytes(), 0o100755) for name, path in files.items()]
    entries += [
        (
            "fl2000-enumerate",
            (ROOT / "userspace/fl2000-enumerate.sh").read_bytes(),
            0o100755,
        ),
        (
            "etc/fl2000/enumerator.conf",
            (ROOT / "userspace/enumerator.conf").read_bytes(),
            0o100644,
        ),
    ]
    return entries
