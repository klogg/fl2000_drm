#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Hardware-free checks of build cleanup, QEMU overlays and analysis input."""

import argparse
import gzip
import json
import lzma
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from compile_commands import merge_databases
from qemu_utils import (
    ROOT,
    add_qemu_arguments,
    cpio_archive,
    dependency_overlay,
    qemu_command,
    write_initramfs,
)
from source_helpers import function, structure


class ToolTests(unittest.TestCase):
    def test_extract_documented_function_with_literal_braces(self):
        definition = '''static int documented(void)
{
 return log("}"); /* Ignore { in comments, too. */
}'''
        source = '''/**
 * documented() - Kernel-doc must not match a function definition
 *
 * Return: 0 on success.
 */
''' + definition
        self.assertEqual(function(source, "documented"), definition)

    def test_extract_structure_ignores_comment_lookalike(self):
        definition = "struct sample {\n int member;\n};"
        source = "/* struct sample {\n int ghost;\n}; */\n" + definition
        self.assertEqual(structure(source, "sample"), definition)

    def test_dependency_overlay(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "first.ko.gz").write_bytes(gzip.compress(b"first module"))
            (root / "second.ko.xz").write_bytes(lzma.compress(b"second module"))
            dependencies = {
                "driver.ko": "first,builtin,second",
                "first.ko.gz": "second",
                "second.ko.xz": "",
            }

            def info(command, **_kwargs):
                if "depends" in command:
                    return dependencies[Path(command[-1]).name] + "\n"
                return (
                    "(builtin)\n"
                    if command[-1] == "builtin"
                    else str(
                        root
                        / (
                            command[-1]
                            + (".ko.gz" if command[-1] == "first" else ".ko.xz")
                        )
                    )
                    + "\n"
                )

            with patch("qemu_utils.subprocess.check_output", side_effect=info):
                entries = dependency_overlay([Path("driver.ko")], "test-kernel")
            self.assertEqual(
                [(name, data) for name, data, _mode in entries[:2]],
                [
                    ("fl2000-dep-second.ko", b"second module"),
                    ("fl2000-dep-first.ko", b"first module"),
                ],
            )
            load = entries[-2][1].decode()
            unload = entries[-1][1].decode()
            self.assertLess(
                load.index("insmod /fl2000-dep-second"),
                load.index("insmod /fl2000-dep-first"),
            )
            self.assertLess(unload.index("rmmod first"), unload.index("rmmod second"))
            self.assertIn("/sys/module/first", load)
            self.assertIn("fl2000-dep-first.ko.loaded", unload)

    def test_clean_stays_in_repository(self):
        with tempfile.TemporaryDirectory() as directory:
            headers = Path(directory) / "headers"
            headers.mkdir()
            (headers / "Makefile").write_text("clean:\n\t@echo kernel-clean\n")
            result = subprocess.check_output(
                ["make", "-n", "-C", str(ROOT), "clean", f"KSRC={headers}"],
                cwd=directory,
                env={**os.environ, "PWD": directory},
                text=True,
            )
            removal = next(
                line for line in result.splitlines() if "Module.symvers" in line
            )
            self.assertIn(str(ROOT / "Module.symvers"), removal)
            self.assertNotIn(str(Path(directory) / "Module.symvers"), removal)

    def test_relocated_qemu_finds_firmware(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "build/qemu-system-x86_64"
            binary.parent.mkdir()
            binary.write_text("#!/bin/sh\nexit 0\n")
            binary.chmod(0o755)
            bios = root / "pc-bios"
            bios.mkdir()
            args = argparse.Namespace(qemu=str(binary), cpu="host", bios_path=None)
            command = qemu_command(args)
            self.assertEqual(command[command.index("-L") + 1], str(bios))
            args.bios_path = root / "missing"
            with self.assertRaises(FileNotFoundError):
                qemu_command(args)

    def test_environment_overrides(self):
        with patch.dict(
            os.environ, {"QEMU": "/custom/qemu", "BASE_INITRAMFS": "/custom/base"}
        ):
            parser = argparse.ArgumentParser()
            add_qemu_arguments(parser)
            args = parser.parse_args([])
            self.assertEqual(args.qemu, "/custom/qemu")
            self.assertEqual(args.base, Path("/custom/base"))

    def test_newc_layout_and_unchanged_base(self):
        entries = [("init", b"#!/bin/sh\n", 0o100755), ("odd", b"abc", 0o100644)]
        archive = cpio_archive(entries)
        self.assertEqual(len(archive) % 512, 0)
        found = []
        offset = 0
        while True:
            self.assertEqual(archive[offset : offset + 6], b"070701")
            fields = [
                int(archive[offset + 6 + i * 8 : offset + 14 + i * 8], 16)
                for i in range(13)
            ]
            size, namesize = fields[6], fields[11]
            name = archive[offset + 110 : offset + 109 + namesize].decode()
            offset = (offset + 110 + namesize + 3) & ~3
            data = archive[offset : offset + size]
            offset = (offset + size + 3) & ~3
            if name == "TRAILER!!!":
                break
            found.append((name, data, fields[1]))
        self.assertEqual(found, entries)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = root / "base.gz"
            original = b"compressed-base-with-odd-length"
            base.write_bytes(original)
            boot = root / "boot.cpio"
            write_initramfs(base, entries, boot)
            expected = original + b"\0" * (-len(original) % 4) + archive
            self.assertEqual(boot.read_bytes(), expected)
        with self.assertRaises(ValueError):
            cpio_archive([("../escape", b"", 0o100644)])

    def test_analysis_covers_optional_backend(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            common = {
                "directory": str(root),
                "file": "driver.c",
                "arguments": ["cc", "-DAUTO"],
            }
            optimized = {**common, "file": "frame/bmi2.c"}
            first, second = root / "auto.json", root / "bmi2.json"
            first.write_text(json.dumps([common]))
            second.write_text(
                json.dumps([{**common, "arguments": ["cc", "-DBMI2"]}, optimized])
            )
            merged = merge_databases([first, second])
            self.assertEqual(len(merged), 2)
            self.assertEqual(merged[0]["arguments"], ["cc", "-DAUTO"])
            self.assertEqual(merged[1]["file"], str(root / "frame/bmi2.c"))


if __name__ == "__main__":
    unittest.main()
