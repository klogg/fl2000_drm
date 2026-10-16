# Driver architecture

The bridge module is named `it66121.ko`. The FL2000 driver discovers the bridge
on its own adapter at I2C address 0x4c or 0x4d; no `i2c_bus_num` parameter is
needed. Each dongle has its own bridge client and DRM device.

The bridge core is rebased on Linux v7.0's
[IT66121 driver](https://github.com/torvalds/linux/blob/v7.0/drivers/gpu/drm/bridge/ite-it66121.c).
It uses upstream atomic bridge state, input-format negotiation, AFE programming,
AVI infoframes, HPD detection, and interrupt acknowledgment. FL2000 adaptations
retain USB component lifetime, the one-cycle parallel input delay, HDMI/DVI
selection, aligned DWORD I2C/EDID reads, and restoration after encoder resets.
Bridge registers are uncached because FL2000 controls their resets. Polling
reads HPD without acknowledging interrupts; the USB event path owns
acknowledgment. Normal connector detection uses HPD events from the USB
interrupt worker instead of periodic polling. The worker updates cached DRM
connector status before notifying clients. Endpoint errors enable
connect/disconnect polling as a fallback; successful interrupt restart restores
HPD-only detection.

`drm_bridge_connector_init()` supplies connector detection, EDID handling,
and HPD delivery. In-tree Kconfig selects its helpers. External builds can
compile missing helpers from checksum-pinned, unmodified kernel sources.
The connector retains bridge references until the final DRM device release;
a DRM-managed module reference keeps the bridge callbacks available until then.
Detection and EDID callbacks check the requesting connector's unplug state,
even after the bridge has been detached or attached to a replacement DRM device.
The USB-created bridge needs no Device Tree graph, regulator devices, or separate
reset GPIO; the FL2000 driver owns power/reset.

Each adapter owns its lifecycle mutex and explicit streaming, interrupt, and
I2C resource pointers. USB core parent-device locking serializes initial state
publication. Independent adapters do not share a lifecycle lock.

## Atomic display pipeline

The display pipeline uses an explicit primary plane, CRTC, and TMDS encoder.
CRTC-only atomic commits include the primary plane in validation and updates.
Plane checks enforce fullscreen output without scaling or positioning, and
GEM shadow-plane state retains framebuffer mappings from preparation through
cleanup. Update and enable callbacks read the state belonging to their commit.
The encoder attaches the IT66121 bridge through the existing component path.

## Frames and transport

The driver accepts linear XRGB8888, RGB888 and RGB565 framebuffers. XRGB8888 is
converted into RGB888 or RGB565 according to estimated USB bandwidth. Native
RGB888 and RGB565 retain their colour representation and only undergo wire-word
reordering. RGB888 uses B, G, R bytes in memory; RGB565 uses little-endian
words. Native RGB888 modes are rejected if the estimated bandwidth only supports
RGB565. Active framebuffer format changes require an atomic commit with
ALLOW_MODESET, so old transfers are drained before the device's pixel format is
changed. Both paths preserve framebuffer offsets and pitch and swap dwords in
each eight-byte wire word across the full frame. The FL2000 compression protocol
is not used. Compression is deferred because host-side encoding adds CPU work.
Future support will be optional, with the current uncompressed path remaining
the default. Its purpose would be to reduce USB bandwidth when that trade-off is
useful.

Three reusable bulk URBs transmit immutable frames. USB noncoherent allocations
retain their DMA mappings across submissions; USB core handles cache
synchronization. If that allocation fails, the driver falls back to page-based
scatter-gather, with separate mapping metadata for each URB. Repeated frames
avoid rebuilding unchanged scatter-gather entries. Both paths reuse unchanged
pixels without a copy. Five buffers allow three distinct in-flight frames, the
newest frame and a free conversion destination. Controller scatter-gather
support is required only for the page-based fallback; contiguous DMA allocations
also work on controllers without it. Queue errors stop transmission; a later
framebuffer update or modeset can drain the old queue and clear the endpoint
halt before restarting it.

Mode validation checks hardware timing field sizes, the 25–148.5 MHz input clock
range used by the upstream IT66121 driver and estimated USB payload capacity.
The bandwidth estimate uses active pixels and refresh timing; sustained
throughput still requires measurements. Interlaced, double-scan, stereoscopic
and fractional source rectangles are rejected.

The device does not expose a physical scanout interrupt. DRM flip events
therefore report software commit completion, not physical vblank or confirmed
presentation. Damage clips update only the regions each reusable wire buffer has
missed. A buffer accumulates damage while it is in flight and is repaired only
after it is free; no submitted buffer is modified and no full-frame copy is
needed. Initial buffers, new framebuffers, mapping/layout changes and modesets
use a full rebuild. Crop-relative damage preserves global eight-byte wire-word
ordering, including odd widths and padding. Full and partial updates share
packed conversion and 64-bit reordering routines.

## CPU backends

In-tree builds select the frame backend through Kconfig. External builds select
it at build time with `FL2000_FRAME_BACKEND=auto` (default), `ssse3`, `bmi2`,
or `generic`. Auto
builds the SSSE3 backend for x86-64 target kernels and the portable backend
elsewhere; selection uses the target kernel configuration, so cross builds do
not inspect the build host CPU. For example, `make FL2000_FRAME_BACKEND=generic`
forces the portable implementation. The x86 backend checks CPU SSSE3 support and
FPU context availability before each bounded SIMD section, falling back to
scalar processing on unsupported CPUs, small spans, and row/damage edges. Only
its computation object is compiled with SIMD enabled. FPU protection ends before
rescheduling.

The SSSE3 routines combine XRGB8888 conversion with FL2000 wire reordering;
native RGB888/RGB565 retain portable reordering because vector reordering did
not provide a repeatable improvement in the complete USB workload. The optional
BMI2 backend uses PEXT on general-purpose registers and requires no FPU
protection; it checks CPU BMI2 support and falls back to portable code when
unavailable. Contiguous XRGB8888 rows are grouped into bounded spans of up to
4096 pixels, reducing per-row dispatch and FPU transitions while preserving
padding and crops. New backends implement the two prefix-processing functions in
`fl2000_frame.h`, add their objects to Kbuild and their architecture selection
to Kconfig, and
define `FL2000_FRAME_ACCELERATED`. Returning zero uses the existing portable
code. Backend implementations must preserve complete wire words, arbitrary
source alignment, and bounded protected sections. `tests/test_logic.py` runs
portable, SSSE3 and BMI2 routines against independent byte references, including
damage edges, chunk boundaries and runtime fallback. QEMU hardware tests default
to `-cpu host`; `--cpu` can override the guest model.
