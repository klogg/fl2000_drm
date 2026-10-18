# Hardware constraints and QEMU passthrough

Physical testing has used one FL2000 adapter and an 800x480 HDMI monitor through
patched QEMU/KVM. Register observations below describe that device. Broader
board coverage and image-fidelity checks remain in the
[validation backlog](DEVELOPMENT.md#validation-backlog).

## Observed device behavior

Base EDID and its checksum were verified on the physical display. Extension
segments remain model-tested. DDC access constraints and the required handling
of destructive reads are documented beside the bridge helpers in
[it66121_drv.c](../bridge/it66121_drv.c).

Physical HDMI unplug/reconnect delivered USB interrupts and bridge
acknowledgment, restored EDID and the framebuffer console, and continued to
work across modesets. The exact board wiring and event-source pin have not
been independently measured. Some register-manual semantics disagree with
this device's behavior; those conflicts are recorded beside the definitions
in [fl2000_registers.h](../fl2000_registers.h).

The [vendor driver](https://github.com/FrescoLogic/FL2000) is a useful reference
for controller setup. Software fault injection tests recovery logic; it does
not reproduce physical endpoint or electrical faults. Broader controllers,
displays, native bridge boards and system PM remain in the validation backlog.

## QEMU 11.1.2 workaround

The tested QEMU 11.1.2 descriptor parser aborts on FL2000's duplicate bulk
endpoint addresses. Interrupt endpoint 0x83 is then unconfigured and returns
immediate STALL responses. The local workaround keeps the first endpoint mapping
and continues parsing for USB ID `1d5c:2000` only.

Apply it to an extracted QEMU 11.1.2 tree:

```sh
FL2000_SRC=/path/to/fl2000_drm
cd /path/to/qemu-11.1.2
patch -p1 < "$FL2000_SRC/patches/qemu-11.1.2-fl2000-duplicate-endpoints.patch"
./configure --target-list=x86_64-softmmu --enable-kvm --enable-libusb \
  --disable-docs --disable-tools --disable-guest-agent
ninja -C build qemu-system-x86_64
```

Select the executable with `QEMU=/path/to/qemu/build/qemu-system-x86_64` when
running `scripts/emulate.sh`. Launchers infer adjacent `pc-bios` firmware for
source builds; `--bios-path PATH` overrides it. The patch neither changes device
descriptors nor replaces the system QEMU installation. Check other QEMU versions
independently rather than assuming this version-specific patch applies.
