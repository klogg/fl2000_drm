# Hardware notes and QEMU workaround

These notes retain the investigation history, including failed configurations.
They are not a sequence of register writes to repeat on a working device.
Physical validation used one FL2000 adapter and an 800x480 HDMI monitor through
patched QEMU/KVM. Visual output was not independently confirmed.

## EDID and interrupts

EDID uses aligned four-byte I2C transfers. Status reads retain the accompanying
FIFO byte; commands use full-word writes without read-modify-write. Physical
FL2000 testing confirmed a three-byte FIFO offset even with this transport.
Reads request three preceding bytes and up to 29 payload bytes, wrapping the
8-bit offset within the selected EDID segment. No header bytes are synthesized.
The readiness timeout ends when DONE is observed; slow USB transfers can then
drain the finite payload without falsely timing out. Base EDID and its checksum
were verified on an 800x480 monitor; later extension segments remain
model-tested.

Interrupt reception starts after initial framebuffer setup and schedules a
status check immediately. A pending event left unread during stop/reset can
suppress further notifications even though a USB request is waiting. The initial
worker acknowledges that status and preserves its hotplug notification. Tests
cover startup work, idempotent enable, USB errors, and cancellation. A
two-second polarity stimulus is used for hardware diagnostics; the earlier
half-second stimulus was unreliable. Controlled interrupt completions and
worker/bridge handling passed at startup, after format-switch modesets, and
across three driver reloads in patched QEMU. Before the detection configuration
fix, physical HDMI hotplug did not deliver a USB interrupt in either the
pre-rebase or rebased bridge tests. With polling paused, unplugging changed
IT66121 HPD low and latched its HPD interrupt, while FL2000 reported no
HDMI-change event. Successful completions are now rearmed by the worker after
device/bridge status processing, matching the vendor ordering; that change alone
did not fix hotplug.

Source comparison with
[Fresco Logic's original driver](https://github.com/FrescoLogic/FL2000) found a
different hotplug path: its worker handles EDID/external-monitor change bits
31/30 of 0x8000, ignoring HDMI-change bit 26. It enables I2C detection, waits
750 ms, then enables external-monitor detection; it does not configure 0x8064
polarity/debounce or implement IT66121 HPD interrupt handling. The vendor forces
VGA connection only around HDMI mode programming. Permanent force-connect in
this driver hid disconnection. In a guest with HDMI unplugged, clearing
force-connect and restarting detection with the PLL running made FL2000 report
disconnected; physical reconnection then delivered a USB IRQ and the bridge
acknowledged HPD. Reconnection's modeset restored forced connection, so the
configuration now keeps force-connect clear across modesets. Probe and resume
establish a 25 MHz PLL clock before detection and I2C bridge discovery;
detection restarts after mode resets with the vendor's 750 ms EDID settling
interval before external-monitor enable. The persistent change passed a full
physical unplug/reconnect cycle in patched QEMU with connector polling disabled:
both transitions delivered USB IRQs and bridge acknowledgment, DRM changed
disconnected/connected, and the 128-byte EDID returned. Force-connect remained
clear after reconnection, native-format modesets, and a driver reload. The
verified configuration used active-low push-pull INT#; bridge enable now
explicitly selects that output mode after resets. Visual output on the monitor
was not reported in this test.

With USB interrupt URBs disabled in an isolated guest build, changing FL2000
polarity set both pending and HDMI-event flags in 0x8000; the next read cleared
them. Changing IT66121 output polarity did not set those flags across FL2000
active-high/active-low and debounce enabled/disabled combinations. Before
correcting detection, both open-drain and push-pull configurations failed the
physical unplug test. Bit 27 of 0x8000 is documented as the HDMI-enable pin, not
a raw INT# level; these results do not prove a disconnected INT# wire. Physical
hotplug delivery now works with the corrected configuration; the exact board
wiring and which FL2000 input reports the event were not measured. Diagnostic
output controls exist only in temporary test builds and are not included in the
driver.

Interrupt setup explicitly enables EDID and external-monitor IRQs (0x803c bits
2/3) and clears bit 4, rather than relying on reset values. On the tested
device, bit 4 set keeps pending/HDMI-event flags latched across reads; bit 4
clear allows the next read to acknowledge them and restores controlled IRQ
delivery. This is the opposite of the register manual's description of bit 4.
The HDMI control register layout follows the datasheet: interrupt enable is bit
19 and active-high polarity is bit 20 of 0x8064. Bridge enable restores the HPD
mask and active-low push-pull output after resets. Polling does not acknowledge
HPD.

Stall recovery runs in a worker, serialized against stop, and gives up after
three retries. QEMU 11.1.2's USB descriptor parser stops on the FL2000's
duplicate bulk endpoint addresses and leaves interrupt endpoint 0x83
unconfigured, producing immediate STALL responses. The local QEMU workaround
below skips those duplicates for USB ID 1d5c:2000 and allows endpoint 0x83 to be
configured.

## QEMU 11.1.2 workaround

The tested QEMU 11.1.2 build aborts endpoint parsing on the
device's duplicate endpoints. Apply the local workaround to an extracted QEMU
11.1.2 source tree; it preserves the first endpoint mapping and continues
parsing only for USB ID `1d5c:2000`:

```sh
FL2000_SRC=/path/to/fl2000_drm
cd /path/to/qemu-11.1.2
patch -p1 < "$FL2000_SRC/patches/qemu-11.1.2-fl2000-duplicate-endpoints.patch"
./configure --target-list=x86_64-softmmu --enable-kvm --enable-libusb \
  --disable-docs --disable-tools --disable-guest-agent
ninja -C build qemu-system-x86_64
```

Select the executable with `QEMU=/path/to/qemu/build/qemu-system-x86_64` when
calling `scripts/emulate.sh`. The launchers infer adjacent `pc-bios` firmware
for source builds; `--bios-path /path/to/firmware` overrides that selection. The
workaround does not modify the device's descriptors or replace the system QEMU
installation.
