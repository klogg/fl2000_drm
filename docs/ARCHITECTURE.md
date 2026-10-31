# Driver architecture

## Discovery and attachment

FL2000 owns a USB transport, an I2C adapter and a DRM display pipeline. During
initial controller setup, status bit 27 (the hardware HDMI-enable pin) selects
the output independently of monitor presence and bridge enumeration. A low pin
attaches an integrated VGA bridge and immediately registers the DRM device.
A high pin waits for a bridge component on its own I2C adapter. A udev-activated
service identifies configured HDMI bridges and creates an I2C client through
`new_device`; see [enumeration](ENUMERATION.md). Failed bridge discovery never
falls back to VGA. The pin is named in the vendor driver and is high in recorded
HDMI captures and low on the tested physical VGA adapter.

The supplied IT6610/21/22 driver derives from the [Linux v7.0 bridge
driver][upstream-bridge]. It supports two attachment contexts:

| Context | Configuration | Attachment | IRQ |
| --- | --- | --- | --- |
| OF board | Graph, supplies, reset GPIO | DRM graph | `client->irq` |
| No OF node | Adapter software node | Component bind | Adapter input 0 |

OF means Device Tree. FL2000 on x86 uses the second context and needs no OF
node or Device Tree support. The core creates a software node on its I2C
adapter, then registers an IRQ domain keyed by that node. The bridge reads the
adapter properties and waits for the domain before registering its component.
A missing domain returns `-EPROBE_DEFER`; polling-only firmware-less
attachment is not supported. An OF node selects the native graph path instead.

Component matching checks the client's owning I2C adapter. During binding,
FL2000 passes a `struct drm_encoder *`; the provider requests its mapped IRQ
and calls `drm_bridge_attach()` with `DRM_BRIDGE_ATTACH_NO_CONNECTOR`. This
bind contract is local to these drivers. Using the component and IRQ
frameworks does not make an arbitrary upstream bridge compatible: other
providers must implement that contract and support the controller's wiring and
transfer constraints.

`drm_bridge_connector_init()` supplies detection, EDID and HPD integration.
In-tree Kconfig selects its dependencies. External builds can compile missing
helpers from checksum-pinned, unmodified kernel sources.

[upstream-bridge]: https://github.com/torvalds/linux/blob/v7.0/drivers/gpu/drm/bridge/ite-it66121.c

## Adapter hardware description

FL2000 supplies transport granularity, pixel input wiring and reset ownership
through its I2C adapter software node. The property names and FL2000 values
are documented in the `FL2000 adapter hardware description` kernel-doc section
in [fl2000_i2c.c](../fl2000_i2c.c). Defaults, validation and OF/component selection
are documented at `it66121_probe_properties()` in
[the bridge](../bridge/it66121_drv.c).

These are local module interfaces, not upstream firmware bindings. Consumers
read the adapter directly; I2C clients do not inherit them automatically.
The userspace candidate configuration does not set them. Standard I2C quirks
separately describe transfer lengths; destructive aligned reads remain an
explicit constraint. See the
[hardware observations](HARDWARE.md#observed-device-behavior).

## Bridge programming and interrupts

The bridge shares its register map, EDID transaction flow, modeset
programming, IRQ handler and HPD reporting across attachment contexts.
Registers are uncached because the display controller can reset the bridge.
Native OF boards retain regulator/reset handling, IT6610/21/22 identification,
12/24-bit input, ordinary I2C access and HDMI audio support. Native-board
validation remains required; FL2000 passthrough does not test those
configurations.

Mode programming runs once in atomic enable, after the CRTC has configured the
display controller. The bridge selects HDMI or DVI from the connector state
and programs input format, AFE and infoframes. If `display-controller-reset`
is set, it restores the configured IRQ output and the framework's HPD mask
before unmuting. For FL2000 this selects active-low push-pull output. Polling
reads HPD without acknowledging it; the IRQ handler owns acknowledgment and
calls `drm_bridge_hpd_notify()` on an HPD event.

USB notifications reach the bridge through a nested Linux IRQ, with I2C work
in sleepable context. Connector polling provides a fallback when the interrupt
path fails. The domain, worker and provider lifetimes are coupled; their
synchronization and retry rules are documented at `struct fl2000_interrupt`
and the interrupt entry points in [fl2000_interrupt.c](../fl2000_interrupt.c).

The native VGA bridge reads monitor presence from status bit 29 and EDID from
DDC address 0x50 using four-byte controller commands. Extended reads select
address 0x30's segment pointer and restore zero under the adapter bus lock.
A failed restoration is retried before subsequent base reads; ordinary base
reads do not require segment-pointer support. EDID validation uses
`drm_edid_read_custom()` synchronously, without a cache. Native USB notifications
rescan connector state even if detection already consumed the event bits, and
never dispatch the external bridge IRQ. Atomic disable stops DAC output while
preserving monitor detection; normal CRTC programming re-enables it.

## Ownership and teardown

Each adapter owns its lifecycle lock and transport resources. The
`struct fl2000_device` documentation in [fl2000.h](../fl2000.h) describes state
ownership. A component bind group (HDMI) or explicit devres bind group (VGA)
owns the initial DRM reference; open files can retain the allocation after
unbind. HDMI additionally retains its provider module. The native bridge is
reference-counted, and its callbacks guard USB access with `drm_dev_enter()`
so retained connectors cannot use resources released after unplug. See
`fl2000_drm_bind()` and `fl2000_drm_cleanup()` in [fl2000_drm.c](../fl2000_drm.c)
for the lifetime and teardown contracts.

Application reset and detection restart run only during initial probe, before
I2C adapter registration, and system resume, while display, polling and IRQ
work are quiescent and child devices and userspace have not resumed. Ordinary
modesets drain streaming and reprogram display registers without restarting
the shared I2C transaction engine. The adapter bus lock excludes individual
transfers during lifecycle setup; it cannot protect a complete bridge DDC
sequence across several transfers.

## Atomic pipeline and frame preparation

The atomic pipeline separates validation and preparation from hardware commit.
That permits allocation failures to leave the current display running. Standard
DRM shmem GEM objects own framebuffer storage; transport buffers belong to the
USB stream. The source-level atomic contract is documented in
[fl2000_drm.c](../fl2000_drm.c), and wire representation, ownership and damage
tracking in [fl2000_streaming.c](../fl2000_streaming.c).

Imported framebuffers and I/O-memory mappings are read through a reusable
16 KiB staging buffer with aligned `drm_memcpy_from_wc()` spans. Prefixes and
tails use mapping-aware copies without reading outside the requested pixels.
The original source mapping remains the damage-history key; staging does not
invalidate retained wire images. Native system-memory framebuffers retain
direct conversion. The standard shmem helper supplies CPU-only PRIME imports
on Linux 7.3; older helpers retain the USB-controller DMA device for imports.

Damage handling reduces host preparation work while the transport continues
sending complete frames. Native framebuffer formats avoid color conversion,
which makes userspace format choice relevant to CPU cost. Flip completion
provides software synchronization; it is not evidence of physical presentation.
See [profiling](PROFILING.md) for measurement scope and results.

## CPU backends

Kconfig selects the in-tree frame backend. External builds accept
`FL2000_FRAME_BACKEND=auto`, `ssse3`, `bmi2` or `generic`. Auto selects SSSE3
for x86-64 target kernels and portable code elsewhere, using the target kernel
configuration rather than the build host CPU.

SSSE3 combines XRGB8888 conversion with wire reordering. It checks CPU
features and FPU context availability and protects bounded SIMD sections with
`kernel_fpu_begin/end()`. Only its computation object enables SIMD
compilation. BMI2 uses PEXT in general-purpose registers and needs no FPU
protection. Both fall back to portable code when features are unavailable or
spans are too small. Native RGB888/RGB565 reordering is portable in every
backend.

New backends implement the kernel-doc contract in
[fl2000_frame.h](../fl2000_frame.h), add Kbuild objects and Kconfig architecture
constraints, and define `FL2000_FRAME_ACCELERATED`. The raw SSSE3/BMI2 routines
have stricter span and CPU/FPU preconditions documented in their headers.
`tests/test_logic.py` checks byte ordering, edges, chunk boundaries and runtime
fallback against independent references.
