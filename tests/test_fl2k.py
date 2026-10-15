#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Replay synthetic usbmon traffic through the real Wireshark Lua API."""

import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def fixture():
    packets = []

    def urb(ident, kind, transfer, endpoint, payload=b"", setup=None):
        header = struct.pack(
            "<QBBBBHBBqiiII8siiII",
            ident,
            ord(kind),
            transfer,
            endpoint,
            2,
            2,
            0 if setup else 45,
            0,
            0,
            0,
            0,
            len(payload),
            len(payload),
            setup or bytes(8),
            0,
            0,
            0,
            0,
        )
        packets.append(header + payload)

    # Enumeration teaches Wireshark the real product ID; no class override.
    urb(1, "S", 2, 0x80, setup=struct.pack("<BBHHH", 0x80, 6, 0x100, 0, 18))
    descriptor = struct.pack(
        "<BBHBBBBHHHBBBB", 18, 1, 0x300, 0, 0, 0, 9, 0x1D5C, 0x2000, 0x100, 1, 2, 3, 1
    )
    urb(1, "C", 2, 0x80, descriptor)
    urb(2, "C", 1, 0x83, b"\x01")
    urb(3, "C", 1, 0x83)
    urb(4, "C", 1, 0x83, b"\xff")
    urb(
        5,
        "S",
        2,
        0,
        b"\x78\x56\x34\x12",
        struct.pack("<BBHHH", 0x40, 0x41, 0, 0x8028, 4),
    )
    urb(6, "S", 2, 0x80, setup=struct.pack("<BBHHH", 0xC0, 0x40, 0, 0x8020, 4))
    urb(6, "C", 2, 0x80, b"\x00\x00\x00\x80")
    urb(7, "S", 2, 0, setup=struct.pack("<BBHHH", 0x40, 0x99, 0, 0x8020, 0))
    urb(7, "C", 2, 0x80, b"\x00")
    urb(8, "S", 3, 1, b"\x01\x02\x03\x04")
    data = bytearray(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 220))
    for number, packet in enumerate(packets, 1):
        data += struct.pack("<IIII", number, 0, len(packet), len(packet)) + packet
    return data


def replay(capture, scripts, options):
    command = ["tshark"]
    for index, (script, arguments) in enumerate(scripts, 1):
        command += ["-X", f"lua_script:{script}"]
        for argument in arguments:
            command += ["-X", f"lua_script{index}:{argument}"]
    command += ["-r", str(capture), *options]
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    assert "Lua: Error" not in result.stderr, result.stderr
    assert "Lua WARNING" not in result.stderr, result.stderr
    return result.stdout


def main():
    if not shutil.which("tshark"):
        raise SystemExit("TShark with Lua support is required")
    if os.geteuid() == 0:
        raise SystemExit("Run the TShark regression as a regular user")
    with tempfile.TemporaryDirectory(prefix="fl2000-lua-") as directory:
        capture = Path(directory) / "usb.pcap"
        capture.write_bytes(fixture())
        driver = ROOT / "scripts/fl2k.lua"
        output = replay(
            capture,
            [(driver, [])],
            [
                "-T",
                "fields",
                "-e",
                "frame.number",
                "-e",
                "fl2k.reg_op",
                "-e",
                "fl2k.reg_addr",
                "-e",
                "fl2k.reg_value",
                "-e",
                "fl2k.irq",
                "-e",
                "fl2k.fb",
                "-e",
                "_ws.expert.message",
            ],
        )
        rows = {
            int(line.split("\t")[0]): line.split("\t")[1:]
            for line in output.splitlines()
        }
        assert rows[3][3] == "0x01", rows[3]
        assert rows[4][3] == "", rows[4]
        assert rows[5][3] == "0xff", rows[5]
        assert rows[6][:3] == ["0x41", "0x8028", "0x12345678"], rows[6]
        assert rows[8][2] == "0x80000000", rows[8]
        assert rows[9][0] == "0x99", rows[9]
        assert rows[10][2] == "", rows[10]
        assert rows[11][4] == "01 02 03 04", rows[11]
        assert all(not row[5] for row in rows.values()), output
        callbacks = replay(
            capture, [(ROOT / "tests/fl2k_headless.lua", [driver])], ["-q"]
        )
        assert "HEADLESS_GUI_CALLBACKS_PASS" in callbacks, callbacks
    print("FL2000 Lua dissection and analysis callbacks: PASS")


if __name__ == "__main__":
    main()
