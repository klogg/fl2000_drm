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

## Bulk transfer queueing

The tested QEMU serializes bulk OUT, limiting synthetic 1080p60 delivery to
approximately 30 USB frames/s. The optional
[bulk-OUT pipeline patch](../patches/qemu-11.1.2-bulk-out-pipeline.patch) allows
queued transfers and restores approximately 60/s with the normal three-frame
driver queue. Apply it after the descriptor workaround, then rebuild QEMU.
It adds an opt-in `bulk-out-pipeline` property; the default remains disabled.

The host USBFS memory allowance must cover queued payloads and transfer
accounting. Three 1080p RGB888 payloads alone occupy 18,662,400 bytes, exceeding
the usual 16 MiB allowance. The throughput measurements used 64 MiB. Set this
on the host before starting QEMU:

```sh
echo 64 | sudo tee /sys/module/usbcore/parameters/usbfs_memory_mb
QEMU=/path/to/qemu/build/qemu-system-x86_64 \
  scripts/emulate.sh --bulk-out-pipeline
```

The shared Python launchers accept the same flag. For a direct QEMU command,
add `bulk-out-pipeline=on` to the `usb-host` device, or use
`-global usb-host.bulk-out-pipeline=on`. Unpatched QEMU rejects this property.

The patch releases requests after failed submissions. A failed pipelined OUT
submission falls back to serialized processing until endpoint reconfiguration,
preserving completion order. Retrying also resets the copied-byte count.
Injected allocation failure and removal during 1080p transfers exercise these
paths; this remains a locally tested QEMU patch, not an upstream guarantee.

See the [transport results](PROFILING.md#transport-diagnostics)
for throughput, recovery tests and remaining validation. The attached monitor
advertises only 800x480, so synthetic 1080p USB throughput does not validate the
image. LBUF overflow still appears at full transfer rate and requires separate
investigation; the pipeline patch does not implement controller recovery.
