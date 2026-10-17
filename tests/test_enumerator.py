#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise the real shell enumerator against synthetic I2C/sysfs devices."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROGRAM = ROOT / "userspace/fl2000-enumerate.sh"
CONFIG = """adapter_name FL2000 bridge I2C bus
bind_timeout 1
bridge example_bridge example_module example_driver 0x38,0x39 0x00:0xff:0x54,0x03:0x0f:0x06
"""


class EnumeratorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sysfs = self.root / "sys"
        self.devices = self.sysfs / "bus/i2c/devices"
        self.devices.mkdir(parents=True)
        self.adapter = self.sysfs / "devices/usb2/i2c-7"
        self.adapter.mkdir(parents=True)
        (self.devices / "i2c-7").symlink_to(self.adapter)
        (self.adapter / "name").write_text("FL2000 bridge I2C bus\n")
        (self.adapter / "new_device").write_text("")
        self.client = self.devices / "7-0039"
        self.config = self.root / "enumerator.conf"
        self.config.write_text(CONFIG)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.log = self.root / "calls"
        self.log.write_text("")
        self.env = {
            **os.environ,
            "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
            "SYSFS_ROOT": str(self.sysfs),
            "LOCK_ROOT": str(self.root / "locks"),
            "TEST_LOG": str(self.log),
            "TEST_MODE": "success",
        }
        self.tool(
            "i2ctransfer",
            """printf 'probe %s\n' "$*" >> "$TEST_LOG"
[ "${TEST_MODE}" != malformed ] || { echo unexpected; exit 0; }
case "$3:$4" in
 w1@0x39:0x00) echo 0x54 ;;
 w1@0x39:0x03) echo 0xa6 ;;
 *) exit 1 ;;
esac
""",
        )
        self.tool(
            "modprobe",
            """[ "$1" = -- ] || exit 2
printf 'module %s\n' "$2" >> "$TEST_LOG"
[ "$TEST_MODE" != module-failure ] || exit 1
mkdir -p "$SYSFS_ROOT/module/$(echo "$2" | tr '-' '_')"
if [ "$2" = example_module ]; then
 client="$SYSFS_ROOT/bus/i2c/devices/${TEST_BUS:-7}-0039"
 mkdir -p "$client"
 echo example_bridge > "$client/name"
 if [ "$TEST_MODE" != deferred ]; then
  mkdir -p "$SYSFS_ROOT/bus/i2c/drivers/example_driver"
  ln -s "$SYSFS_ROOT/bus/i2c/drivers/example_driver" "$client/driver"
 fi
fi
""",
        )
        self.tool("sleep", ":\n")

    def tool(self, name, body):
        path = self.bin / name
        path.write_text("#!/bin/sh\nset -eu\n" + body)
        path.chmod(0o755)

    def run_enumerator(self):
        return subprocess.run(
            [str(PROGRAM), str(self.adapter), str(self.config)],
            env=self.env,
            text=True,
            capture_output=True,
            timeout=5,
            check=False,
        )

    def test_argument_limits_have_no_device_side_effects(self):
        for arguments in ([], [str(self.adapter), str(self.config), "extra"]):
            with self.subTest(arguments=arguments):
                result = subprocess.run(
                    [str(PROGRAM), *arguments],
                    env=self.env,
                    text=True,
                    capture_output=True,
                    timeout=5,
                    check=False,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Expected adapter path", result.stderr)
                self.assertEqual(self.log.read_text(), "")

    def test_client_disappears_while_waiting(self):
        self.config.write_text(CONFIG.replace("bind_timeout 1", "bind_timeout 2"))
        self.env["TEST_MODE"] = "deferred"
        self.tool(
            "sleep",
            'client="$SYSFS_ROOT/bus/i2c/devices/7-0039"\n'
            'rm -- "$client/name"\nrmdir -- "$client"\n',
        )
        result = self.run_enumerator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Adapter or client disappeared", result.stderr)

    def test_identify_before_creation_and_idempotence(self):
        result = self.run_enumerator()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Bound example_bridge at 0x39 on i2c-7", result.stdout)
        self.assertEqual(
            (self.adapter / "new_device").read_text(), "example_bridge 0x39\n"
        )
        calls = self.log.read_text()
        self.assertIn("probe -y 7 w1@0x38 0x00 r1", calls)
        self.assertIn("probe -y 7 w1@0x39 0x03 r1", calls)
        self.assertLess(calls.index("probe"), calls.index("module example_module"))
        self.log.write_text("")
        self.assertEqual(self.run_enumerator().returncode, 0)
        self.assertEqual(self.log.read_text(), "")

    def test_registered_builtin_driver_needs_no_module(self):
        self.client.mkdir()
        (self.client / "name").write_text("example_bridge\n")
        driver = self.sysfs / "bus/i2c/drivers/example_driver"
        driver.mkdir(parents=True)
        (self.client / "driver").symlink_to(driver)
        self.env["TEST_MODE"] = "module-failure"
        result = self.run_enumerator()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.log.read_text(), "")
        self.assertFalse((self.sysfs / "module/example_module").exists())

    def test_preloaded_module_needs_no_modprobe(self):
        self.client.mkdir()
        (self.client / "name").write_text("example_bridge\n")
        (self.sysfs / "module/example_module").mkdir(parents=True)
        self.env["TEST_MODE"] = "module-failure"
        result = self.run_enumerator()
        self.assertIn("preserving it for deferred probing", result.stderr)
        self.assertEqual(self.log.read_text(), "")

    def test_selected_adapter_does_not_modify_another_bus(self):
        other = self.sysfs / "devices/usb3/i2c-8"
        other.mkdir(parents=True)
        (other / "name").write_text("FL2000 bridge I2C bus\n")
        (other / "new_device").write_text("")
        (self.devices / "i2c-8").symlink_to(other)
        self.adapter = other
        self.env["TEST_BUS"] = "8"
        result = self.run_enumerator()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((other / "new_device").read_text(), "example_bridge 0x39\n")
        self.assertEqual((self.devices / "i2c-7/new_device").read_text(), "")
        self.assertIn("probe -y 8", self.log.read_text())

    def test_occupied_address_is_not_probed(self):
        occupied = self.devices / "7-0038"
        occupied.mkdir()
        (occupied / "name").write_text("unrelated\n")
        self.assertEqual(self.run_enumerator().returncode, 0)
        self.assertNotIn("w1@0x38", self.log.read_text())
        self.assertEqual((occupied / "name").read_text(), "unrelated\n")

    def test_deferred_client_is_preserved(self):
        self.env["TEST_MODE"] = "deferred"
        result = self.run_enumerator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("preserving it for deferred probing", result.stderr)
        self.assertTrue(self.client.exists())
        self.assertFalse((self.adapter / "delete_device").exists())

    def test_no_match_does_not_create_client(self):
        self.config.write_text(CONFIG.replace("0x00:0xff:0x54", "0x00:0xff:0x55"))
        result = self.run_enumerator()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.adapter / "new_device").read_text(), "")
        self.assertNotIn("module example_module", self.log.read_text())

    def test_invalid_configuration_has_no_device_side_effects(self):
        invalid = [
            CONFIG.replace("0x38,0x39", "0x00"),
            CONFIG.replace("0xff:0x54", "0x00:0x54"),
            CONFIG.replace("bind_timeout 1", "bind_timeout 99"),
            CONFIG.replace("example_module", "$(touch_injected)"),
            CONFIG + "unknown value\n",
            CONFIG + "adapter_name duplicate\n",
        ]
        for config in invalid:
            with self.subTest(config=config):
                self.config.write_text(config)
                self.assertNotEqual(self.run_enumerator().returncode, 0)
                self.assertEqual(self.log.read_text(), "")
                self.assertEqual((self.adapter / "new_device").read_text(), "")

    def test_non_default_timeout_and_configuration_order(self):
        self.config.write_text(
            CONFIG.replace("bind_timeout 1\n", "") + "bind_timeout 08\n"
        )
        result = self.run_enumerator()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_malformed_transfer_output(self):
        self.env["TEST_MODE"] = "malformed"
        result = self.run_enumerator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Invalid byte", result.stderr)
        self.assertEqual((self.adapter / "new_device").read_text(), "")

    def test_module_failure_does_not_instantiate(self):
        self.env["TEST_MODE"] = "module-failure"
        self.assertNotEqual(self.run_enumerator().returncode, 0)
        self.assertEqual((self.adapter / "new_device").read_text(), "")

    def test_unrelated_adapter_is_rejected(self):
        (self.adapter / "name").write_text("unrelated bus\n")
        self.assertNotEqual(self.run_enumerator().returncode, 0)
        self.assertEqual(self.log.read_text(), "")

    def test_shipped_configuration_and_install(self):
        self.config.write_bytes((ROOT / "userspace/enumerator.conf").read_bytes())
        result = self.run_enumerator()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("Invalid configuration", result.stderr)
        destination = self.root / "install"
        command = ["make", "install-enumerator", f"DESTDIR={destination}"]
        subprocess.run(command, cwd=ROOT, check=True, capture_output=True)
        config = destination / "etc/fl2000/enumerator.conf"
        self.assertEqual(config.read_bytes(), self.config.read_bytes())
        config.write_text("local configuration\n")
        subprocess.run(command, cwd=ROOT, check=True, capture_output=True)
        self.assertEqual(config.read_text(), "local configuration\n")
        self.assertTrue(
            os.access(destination / "usr/libexec/fl2000-enumerate", os.X_OK)
        )
        self.assertTrue((destination / "usr/lib/udev/rules.d/70-fl2000.rules").exists())
        self.assertTrue(
            (destination / "usr/lib/systemd/system/fl2000-enumerate@.service").exists()
        )


if __name__ == "__main__":
    unittest.main()
