# Driver architecture

The FL2000 core registers an I2C adapter and waits for a bridge component on
that adapter. A udev-activated service identifies bridges from external
configuration and creates the I2C client through `new_device`; the core does
not scan chip IDs, select a bridge driver, or require Device Tree. Components
are matched by their owning I2C adapter.
See [userspace enumeration](ENUMERATION.md) for configuration and activation.

Each dongle owns its own adapter and DRM device. Component binding receives
its DRM encoder; the bridge attaches itself using `drm_bridge_attach()`.
The master uses the encoder bridge chain and pins the matched I2C driver's
module until the final DRM device release. There is no shared private header
or bridge payload between the drivers.

The supplied `it66121.ko` is one optional bridge provider. Providers on FL2000
need component binding and an IRQ request in addition to their normal I2C
probe; instantiating an arbitrary upstream I2C bridge alone does not supply it.
The adapter software node identifies its controller as `fresco,fl2000-i2c`
before registration. This identity explicitly selects the transport-specific
DDC access, reset, timing, and component adaptations; absence of Device Tree
does not.
The core publishes no IT66121-specific settings.

The component framework remains responsible for readiness and bind/unbind
ordering. Passing a `struct drm_encoder *` as component bind data is the
agreement between the master and provider; DRM does not automatically impose
that contract on other bridge drivers. The provider requests its IRQ and
attaches its bridge during bind, then frees the IRQ during unbind. The master
stops USB interrupt work and detaches the encoder before provider unbind.

DRM registration currently waits for an external bridge component. Native
VGA through the integrated FL2000 DAC would need a separate output path and
explicit hardware selection; it is not implemented or inferred from failed
bridge discovery.

The bridge core is rebased on Linux v7.0's
[IT66121 driver](https://github.com/torvalds/linux/blob/v7.0/drivers/gpu/drm/bridge/ite-it66121.c).
It uses upstream atomic bridge state, input-format negotiation, AFE programming,
AVI infoframes, HPD detection, and interrupt acknowledgment. FL2000 adaptations
retain USB component lifetime, the one-cycle parallel input delay, HDMI/DVI
selection, aligned DWORD I2C/EDID reads, and restoration after encoder resets.
The driver shares one regmap initializer, EDID callback and block transaction
flow, modeset implementation, IRQ handler, and HPD reporting. Small DDC access
helpers account for aligned transfers and destructive FIFO snapshots; there
is no separate FL2000 EDID algorithm.
Normal firmware-described boards retain OF matching and graph lookup,
regulator/reset control, detected IT6610/21/22 identities, 12/24-bit input,
`client->irq` or polling, ordinary I2C EDID reads, and HDMI audio. Component
registration and transport IRQ lookup apply only to FL2000. Audio codec
resources are released with the parent device.
Bridge mode programming runs once in atomic enable, after the CRTC has reset
and configured FL2000. The bridge restores its interrupt configuration before
unmuting; it has no legacy `mode_set` callback. Generic boards retain the
upstream input clock phase and interrupt configuration. FL2000 restores its
active-low push-pull output and HPD mask according to the framework's last
HPD enable/disable request. Disabling a bridge clears the active connector.
Bridge registers are uncached because FL2000 controls their resets. Polling
reads HPD without acknowledging interrupts; the bridge IRQ handler owns
acknowledgment and calls `drm_bridge_hpd_notify()` for HPD events.

FL2000 creates a one-input IRQ domain keyed by its I2C adapter's software
node, without Device Tree. The bridge finds that domain using the adapter's
fwnode and looks up input zero through standard IRQ-domain APIs. The adapter
owns the node; the FL2000 interrupt resources own the domain and mapping.
Its USB worker calls `handle_nested_irq()` in sleepable context, so the
bridge's normal threaded handler finishes before another interrupt URB is
submitted. The bridge
requests the IRQ during component binding and frees it during unbinding;
userspace I2C `new_device` needs no IRQ extension. IRQ unmask schedules a
bridge rescan so an already-consumed FL2000 event cannot strand a pending IRQ.
Connector selection comes from atomic state when the bridge is enabled.

A per-adapter I2C error counter preserves acknowledgment retry without a
private bridge return value: a failed transfer during dispatch or detection
holds USB rearming, enables connector polling, and schedules a delayed retry.
An unrelated concurrent I2C failure can conservatively cause the same retry.
Retries dispatch the IRQ even if the earlier status read cleared the event.
Endpoint errors also enable polling; successful recovery restores HPD-only
detection. Unbinding stops the USB worker before freeing the bridge IRQ, and
the IRQ mapping and domain are released after component teardown.

`drm_bridge_connector_init()` supplies connector detection, EDID handling,
and HPD delivery. In-tree Kconfig selects its helpers. External builds can
compile missing helpers from checksum-pinned, unmodified kernel sources.
The connector retains bridge references until the final DRM device release;
a DRM-managed module reference keeps the bridge callbacks available until then.
Detection and EDID callbacks check the requesting connector's unplug state,
even after the bridge has been detached or attached to a replacement DRM device.
The bridge instantiated on FL2000 needs no Device Tree graph, regulators, or
separate reset GPIO; the FL2000 driver owns power/reset.

Each adapter owns its lifecycle mutex and explicit streaming, interrupt, and
I2C resource pointers. USB core parent-device locking serializes initial state
publication. Independent adapters do not share a lifecycle lock.

## Atomic display pipeline

The display pipeline uses an explicit primary plane, CRTC, and parallel RGB encoder.
CRTC-only atomic commits include the primary plane in validation and updates.
Plane checks enforce fullscreen output without scaling or positioning, and
GEM shadow-plane state retains framebuffer mappings from preparation through
cleanup. Update and enable callbacks read the state belonging to their commit.
PLL clock quantization and horizontal-total adjustment run in the CRTC
`mode_fixup` callback during modeset checking. They preserve bridge timing
adjustments and refresh derived CRTC timings. Plane validation only reads the
resulting mode when checking geometry and USB bandwidth. The encoder attaches
a bridge through the component framework.

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

FL2000 mode validation checks hardware timing field sizes, the 25–148.5 MHz
input clock
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
padding and crops. New backends implement the conversion prefix function in
`fl2000_frame.h`, add their objects to Kbuild and their architecture selection
to Kconfig, and
define `FL2000_FRAME_ACCELERATED`. Returning zero uses the existing portable
code. Backend implementations must preserve complete wire words, arbitrary
source alignment, and bounded protected sections. `tests/test_logic.py` runs
portable, SSSE3 and BMI2 routines against independent byte references, including
damage edges, chunk boundaries and runtime fallback. QEMU hardware tests default
to `-cpu host`; `--cpu` can override the guest model.
