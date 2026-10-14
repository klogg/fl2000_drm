[![Gitter](https://badges.gitter.im/fl2000_drm/community.svg)](https://gitter.im/fl2000_drm/community?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge)
[![Build](https://github.com/klogg/fl2000_drm/actions/workflows/makefile.yml/badge.svg)](https://github.com/klogg/fl2000_drm/actions/workflows/makefile.yml)
[![Stylecheck](https://github.com/klogg/fl2000_drm/actions/workflows/codingstyle.yaml/badge.svg)](https://github.com/klogg/fl2000_drm/actions/workflows/codingstyle.yaml)
[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=klogg_fl2000_drm&metric=alert_status)](https://sonarcloud.io/dashboard?id=klogg_fl2000_drm)

# Linux kernel FL2000DX/IT66121FN dongle DRM driver

Clean re-implementation of FrescoLogic FL2000DX DRM driver and ITE Tech IT66121F driver, allowing to enable full display controller capabilities for [USB-to-HDMI dongles](https://www.aliexpress.com/item/32821739801.html?spm=a2g0o.productlist.0.0.14ee52fb8rFfu5) based on such chips in Linux

### Building driver

Check out the code and type
```
make
```
For isolated hardware testing, supply a bootable base initramfs for the target
kernel with a shell, module-loading tools, and DRM dependencies. This image is
not included in the repository. Set `BASE_INITRAMFS` to its path (the default is
`base-initramfs.cpio.gz` in the repository root), then use physical USB port
passthrough:

```sh
./scripts/emulate.sh --hostbus 2 --hostport 2
```

Inside the guest, run `/fl2000-startup`. The launcher appends the modules and this
helper to the supplied base initramfs. Startup loads IT66121 before FL2000, is
idempotent, and refuses loading outside the marked test guest. No host USB controller is unbound. A local patched QEMU build is
selected when available, with an explicit firmware path so moving it does not
break BIOS lookup. Override `QEMU`, `KVER`, `MODULE_DIR`, `BASE_INITRAMFS`, or
`QEMU_OUTPUT_DIR`, or pass the launcher's command-line options. Logs default to
`.qemu-build/fl2000-tests` and survive a reboot.

If signing is required for the target kernel, `./scripts/sign.sh` signs both
modules and propagates failures. It supports `KVER`, `KSRC`, `MODULE_DIR`,
`KEY_DIR`, `SIGN_KEY`, `SIGN_CERT`, and `SIGN_CMD` overrides; run it with access to
your signing key. It resolves module paths from the repository rather than the
calling directory.

The style scripts include the new `frame/` sources and skip generated module
files. They prefer installed kernel tools; missing resources are cached from the
pinned `v7.0` tag (override `KERNEL_TOOLS_TAG`). `clang-format.sh --dry-run` checks
without editing and fails on formatting errors. The USB Lua dissector handles
one-byte interrupts and incomplete captures, and registers its menu only in GUI
Wireshark.

To validate the dissector without a GUI, record a USB capture inside the QEMU
guest using usbmon. Start recording before attaching the physical port so the
capture includes enumeration; then load the driver. Replay the capture with:

```sh
tshark -X lua_script:scripts/fl2k.lua -r capture.pcapng -Y fl2k
```

The headless callback regression also exercises the analysis output, both
statistics buttons, repeated redraw, and capture reset:

```sh
tshark -X lua_script:tests/fl2k_headless.lua \
  -X lua_script1:scripts/fl2k.lua -r capture.pcapng -q
```

Require `HEADLESS_GUI_CALLBACKS_PASS` in its output; TShark can return success
while reporting a Lua callback error. The capture must contain FL2000 register
traffic. Run these commands as a regular user with read access to the capture
and scripts, using a Lua-enabled TShark build. The regression replaces window
widgets with test objects; visual layout still needs GUI testing.

GitHub CI builds the automatic, BMI2, and generic backends in separate Ubuntu
26.04 containers against installed Linux 7.0 headers. It runs the sanitizer
regressions and hardware-free USB replay (`python3 tests/test_fl2k.py`) on a
regular hosted runner; no private initramfs or attached device is required.
Style jobs use pinned Linux 7.0 tools and clang-format 18, preserve diagnostics
as artifacts, and fail on existing style violations. SonarCloud uses a clean
7.0 build and its compilation database; analysis is skipped for fork pull
requests or when `SONAR_TOKEN` is missing.

**NOTE:** This revision targets Linux 7.0. Build tools and headers matching the
kernel are required. Both modules have been compile-tested on x86_64 against
Ubuntu kernels 7.0.0-34 and 7.0.0-38. Older kernels require a separate API port.

To build against a different installed header tree:
```
make KSRC=/usr/src/linux-headers-7.0.0-38-generic
```

The bridge module is named `it66121.ko`. The FL2000 driver discovers the bridge
on its own adapter at I2C address 0x4c or 0x4d; no `i2c_bus_num` parameter is needed.
Each dongle has its own bridge client and DRM device.

The bridge core is rebased on Linux v7.0's
[IT66121 driver](https://github.com/torvalds/linux/blob/v7.0/drivers/gpu/drm/bridge/ite-it66121.c).
It uses upstream atomic bridge state, input-format negotiation, AFE programming,
AVI infoframes, HPD detection, and interrupt acknowledgment. FL2000 adaptations
retain USB component lifetime, the one-cycle parallel input delay, HDMI/DVI
selection, aligned DWORD I2C/EDID reads, and restoration after encoder resets.
Bridge registers are uncached because FL2000 controls their resets. Polling reads
HPD without acknowledging interrupts; the USB event path owns acknowledgment.
Normal connector detection uses HPD events from the USB interrupt worker instead
of periodic polling. The worker updates cached DRM connector status before
notifying clients. Endpoint errors enable connect/disconnect polling as a fallback;
successful interrupt restart restores HPD-only detection.

The local connector wrapper in `fl2000_connector.c` forwards detection, EDID,
and mode validation to bridge callbacks and retains a bridge reference. It avoids
requiring `CONFIG_DRM_BRIDGE_CONNECTOR`, which is unavailable in the tested
distribution kernels. The USB-created bridge needs no Device Tree graph,
regulator devices, or separate reset GPIO; the FL2000 driver owns power/reset.

### Validation

Run the logic regressions with a C compiler and Python 3:
```
python3 tests/test_logic.py
python3 tests/test_edid.py
python3 tests/test_bridge.py
python3 tests/test_interrupt.py
```
The harness compiles actual driver helpers with AddressSanitizer and
UndefinedBehaviorSanitizer. It checks packed pixel data, odd widths, padded and
unaligned native and XRGB input, framebuffer crops, format-switch modesets,
format-specific bandwidth limits, PLL adjustments, timing limits, register masks, I2C validation,
short USB replies, unchanged-mode checks, framebuffer mapping lifetimes, DMA
buffer allocation/fallback, and mocked queue submission, cancellation and recovery.
The EDID harness checks FIFO side effects and later extension segments.
The bridge harness checks HDMI/DVI mode programming, input delay, reset
restoration, HPD acknowledgment, mode limits, and register errors.

For a strict module build:
```
make -C /usr/src/linux-headers-7.0.0-38-generic M="$PWD" W=1 KCFLAGS=-Werror modules
```

These checks do not establish hardware functionality. Before deployment, test
monitor hotplug, display modes, USB disconnect with open DRM handles, independent
interface and bridge unbind/rebind, suspend/resume, USB errors and multiple dongles.

### Display and transport behavior

The driver accepts linear XRGB8888, RGB888 and RGB565 framebuffers. XRGB8888 is
converted into RGB888 or RGB565 according to estimated USB bandwidth. Native
RGB888 and RGB565 retain their colour representation and only undergo wire-word
reordering. RGB888 uses B, G, R bytes in memory; RGB565 uses little-endian words.
Native RGB888 modes are rejected if the estimated bandwidth only supports RGB565.
Active framebuffer format changes require an atomic commit with ALLOW_MODESET,
so old transfers are drained before the device's pixel format is changed. Both
paths preserve framebuffer offsets and pitch and swap dwords in each eight-byte
wire word across the full frame. The FL2000 compression protocol is not used.

Three reusable bulk URBs transmit immutable frames. USB noncoherent allocations
retain their DMA mappings across submissions; USB core handles cache synchronization.
If that allocation fails, the driver falls back to page-based scatter-gather,
with separate mapping metadata for each URB. Repeated frames avoid rebuilding
unchanged scatter-gather entries. Both paths reuse unchanged pixels without a copy.
Five buffers allow three distinct in-flight frames, the newest frame and a free
conversion destination. A host controller with USB scatter-gather support is
required. Queue errors stop transmission; a later framebuffer update or modeset
can drain the old queue and clear the endpoint halt before restarting it.

Mode validation checks hardware timing field sizes, the 25–148.5 MHz input
clock range used by the upstream IT66121 driver and estimated USB payload capacity. The bandwidth estimate uses active
pixels and refresh timing; sustained throughput still requires measurements.
Interlaced, double-scan, stereoscopic and fractional source rectangles are rejected.

The device does not expose a physical scanout interrupt. DRM flip events therefore
report software commit completion, not physical vblank or confirmed presentation.
Damage clips update only the regions each reusable wire buffer has missed. A
buffer accumulates damage while it is in flight and is repaired only after it is
free; no submitted buffer is modified and no full-frame copy is needed. Initial
buffers, new framebuffers, mapping/layout changes and modesets use a full rebuild.
Crop-relative damage preserves global eight-byte wire-word ordering, including
odd widths and padding. Full and partial updates share packed conversion and
64-bit reordering routines. QEMU/KVM profiling at 800x480 with 60 updates/s
measured update-process CPU at approximately 4.0% of one core for XRGB8888, 2.0%
for RGB888, and 1.5% for RGB565. Updating a 64x64 rectangle took essentially the
same CPU as updating the full screen. These are process CPU measurements, including
DRM ioctl work, not total driver CPU; asynchronous work and QEMU passthrough
consume additional CPU. Function profiling measured frame preparation at roughly
560/240/160 microseconds per update respectively. Nested elapsed timings must not
be summed or treated as CPU time. Raw results and methodology are recorded in
[tests/cpu_profile_results.json](tests/cpu_profile_results.json).

A matched post-restart run at 800x480 and 60 updates/s measured 64x64 update
process CPU falling from 9.83% to 1.49% for XRGB8888, 4.07% to 1.33% for RGB888,
and 2.93% to 1.24% for RGB565. Full-screen CPU stayed within about 3% of the
matched full-frame baseline. These figures use the same VM/host conditions and
must not be compared directly to the older absolute values above. Periodic
bridge/I2C reads were absent from all three optimized function profiles. See
[tests/cpu_profile_partial_results.json](tests/cpu_profile_partial_results.json).

The optimized driver also passed a physical HDMI unplug/reconnect cycle in patched
QEMU with normal HPD-only connector detection. Both transitions followed the USB
interrupt worker through bridge acknowledgment and DRM detection, with no periodic
detect calls. Unplug disabled the CRTC; reconnect restored the 128-byte EDID and
active 800x480 framebuffer console. Both modules unloaded cleanly before guest
shutdown. Visual output on the monitor was not independently confirmed.

Frame preparation selects a backend at build time with
`FL2000_FRAME_BACKEND=auto` (default), `ssse3`, `bmi2`, or `generic`. Auto builds the
SSSE3 backend for x86-64 target kernels and the portable backend elsewhere;
selection uses the target kernel configuration, so cross builds do not inspect
the build host CPU. For example, `make FL2000_FRAME_BACKEND=generic` forces the
portable implementation. The x86 backend checks CPU SSSE3 support and FPU context
availability before each bounded SIMD section, falling back to scalar processing
on unsupported CPUs, small spans, and row/damage edges. Only its computation
object is compiled with SIMD enabled. FPU protection ends before rescheduling.

The SSSE3 routines combine XRGB8888 conversion with FL2000 wire reordering;
native RGB888/RGB565 retain portable reordering because vector reordering did not
provide a repeatable improvement in the complete USB workload. The optional BMI2
backend uses PEXT on general-purpose registers and requires no FPU protection;
it checks CPU BMI2 support and falls back to portable code when unavailable.
Contiguous XRGB8888 rows are grouped into bounded spans of up to 4096 pixels,
reducing per-row dispatch and FPU transitions while preserving padding and crops.
New backends implement the
two prefix-processing functions in `fl2000_frame.h`, add their objects and target
selection to the Makefile, and define `FL2000_FRAME_ACCELERATED`. Returning zero
uses the existing portable code. Backend implementations must preserve complete
wire words, arbitrary source alignment, and bounded protected sections.
`tests/test_logic.py` runs portable, SSSE3 and BMI2 routines against independent
byte references, including damage edges, chunk boundaries and runtime fallback.
QEMU hardware tests default to `-cpu host`; `--cpu` can override the guest model.

The final matched run on the i3-6100T at 800x480 and 60 full-frame updates/s
measured update-process CPU as follows (median of two 10-second trials):

| Input | Portable baseline | SSSE3 | BMI2 |
| --- | ---: | ---: | ---: |
| XRGB8888 | 10.84% | 5.31% | 5.54% |
| RGB888 | 4.02% | 4.02% | 4.02% |
| RGB565 | 2.94% | 2.96% | 2.92% |

SSSE3 remains the default x86-64 backend. BMI2 is selectable and needs no FPU
protection, but did not beat SSSE3 in the live XRGB8888 workload. An isolated
kernel benchmark found faster BMI2 RGB565 conversion (110 versus 152 microseconds
for SSSE3 at 800x480); live RGB565 input needs no conversion, so these are separate
results. Kernel sampling attributed about 84% of XRGB8888 kernel samples to
preparation and very few to FL2000 USB submission/completion. QEMU host execution
used roughly 20–24% of one core, with about 11% present before driver loading.
Host instruction sampling was unavailable because of container permissions.

The isolated hot FPU begin/end loop added approximately 26 ns per iteration over
its timer/loop baseline; this excludes the full userspace state transition and
does not establish the cause of the earlier native SIMD regression. Native SIMD
was faster on cached benchmark buffers but did not improve the full USB workload.
Actual SSE/AVX state-preservation checks passed for all three framebuffer formats.
The long profiling run reached its watchdog after the last measurement; the
watchdog budget was corrected and a shorter run completed final mode restoration,
module unload and guest shutdown. Full data, sample counts and methodology are in
[tests/cpu_profile_backend_results.json](tests/cpu_profile_backend_results.json).
Final strict builds passed on Linux 7.0.0-34 and -38. In a physical-device KVM
test using the qemu64 CPU model with SSSE3 and BMI2 disabled, both optimized
modules successfully fell back to portable preparation for all three formats;
module unload and guest shutdown completed cleanly.

The guest profiler separates static-screen, full-frame, small-damage, and traced
workloads; `--quick` runs only the untraced full/small comparison. Build it with:
```
cc -static -O2 -Wall -Wextra -Werror -I/usr/include/drm tests/kms_profile.c -o /tmp/kms-profile
```
Pass it as the QEMU hardware launcher's `--kms-test /tmp/kms-profile` argument;
inside that guest it is named `/kms-smoke`. Run `/kms-smoke --baseline` before
loading the drivers, then mount debugfs and run `/kms-smoke` after loading them.
The workload uses the monitor's preferred mode when 800x600 is unavailable.
The guest's `/proc/stat` totals were noisy in this VM; the saved process CPU and
function profiles are the primary comparison metrics.

`tests/profile_qemu.py` compares supplied modules in one guest, reverses their
order on alternating trials, and records QEMU thread CPU alongside workload
windows. It subtracts `/proc` guest time from QEMU thread CPU to separate host
execution; this accounting has tick granularity and excludes host USB IRQ work
outside QEMU. `tests/kernel_sample.c` samples guest kernel instruction pointers
and call chains around the same workload markers. Sampling and function profiling
run separately from the untraced comparison, and their CPU figures are diagnostic.
The profiler accepts `--duration=N`, `--format=XRGB8888` (or RGB888/RGB565),
`--full-only`, `--static-only`, and `--check-fpu`. The last option verifies SSE/AVX
register preservation across actual framebuffer-update syscalls on x86-64.
Framebuffer mappings remain held from atomic preparation until cleanup, including
overlapping states that use the same framebuffer. PLL searches run only when the
mode changes. XRGB8888 conversion to RGB888 packs four pixels into three dword
writes; conversion to RGB565 packs pairs using integer operations. Native input
uses a 64-bit rotate-and-copy loop, omitting row padding. Odd-width rows and partial wire words retain a scalar
fallback.

To compare computation and PLL helpers without hardware:

```sh
python3 tests/benchmark_cpu.py
```

The userspace benchmark compares portable, SSSE3 and BMI2 XRGB8888 conversion with
tightly packed and padded native inputs, including an experimental native SIMD
candidate. It reports thread CPU time and an estimate for 60 updates per second
on one core. It excludes kernel FPU protection, mapping, USB, DMA and scheduling costs; actual
driver utilization needs measurements with the device attached. Native-format
savings apply when userspace supplies RGB888/RGB565; fbdev still uses XRGB8888.

`python3 tests/benchmark_kernel.py --output /tmp/fl2000-kernel-bench` builds a
temporary benchmark module and loads it only inside hardware-free KVM. It extracts
the actual driver helpers, uses real FPU protection, checks prepared bytes and
measures computation with rotating cached buffers. Those buffers differ from the
live GEM/USB buffers, so this benchmark cannot replace end-to-end measurements.
Framebuffer objects themselves use standard DRM shmem GEM helpers; the five
driver-owned wire buffers are USB transport allocations, not a custom GEM type.

EDID uses aligned four-byte I2C transfers. Status reads retain the accompanying
FIFO byte; commands use full-word writes without read-modify-write. Physical
FL2000 testing confirmed a three-byte FIFO offset even with this transport.
Reads request three preceding bytes and up to 29 payload bytes, wrapping the
8-bit offset within the selected EDID segment. No header bytes are synthesized.
The readiness timeout ends when DONE is observed; slow USB transfers can then
drain the finite payload without falsely timing out. Base EDID and its checksum
were verified on an 800x480 monitor; later extension segments remain model-tested.

Interrupt reception starts after initial framebuffer setup and schedules a status
check immediately. A pending event left unread during stop/reset can suppress
further notifications even though a USB request is waiting. The initial worker
acknowledges that status and preserves its hotplug notification. Tests cover
startup work, idempotent enable, USB errors, and cancellation. A two-second
polarity stimulus is used for hardware diagnostics; the earlier half-second
stimulus was unreliable. Controlled interrupt completions and worker/bridge handling passed at startup,
after format-switch modesets, and across three driver reloads in patched QEMU.
Before the detection configuration fix, physical HDMI hotplug did not deliver a
USB interrupt in either the pre-rebase or rebased bridge tests. With polling paused, unplugging changed IT66121 HPD low
and latched its HPD interrupt, while FL2000 reported no HDMI-change event.
Successful completions are now rearmed by the worker after device/bridge status
processing, matching the vendor ordering; that change alone did not fix hotplug.

Source comparison with [Fresco Logic's original driver](https://github.com/FrescoLogic/FL2000)
found a different hotplug path: its worker handles EDID/external-monitor change
bits 31/30 of 0x8000, ignoring HDMI-change bit 26. It enables I2C detection, waits
750 ms, then enables external-monitor detection; it does not configure 0x8064
polarity/debounce or implement IT66121 HPD interrupt handling. The vendor forces
VGA connection only around HDMI mode programming. Permanent force-connect in this
driver hid disconnection. In a guest with HDMI unplugged, clearing force-connect
and restarting detection with the PLL running made FL2000 report disconnected;
physical reconnection then delivered a USB IRQ and the bridge acknowledged HPD.
Reconnection's modeset restored forced connection, so the configuration now keeps
force-connect clear across modesets. Probe and resume establish a 25 MHz PLL clock
before detection and I2C bridge discovery; detection restarts after mode resets
with the vendor's 750 ms EDID settling interval before external-monitor enable.
The persistent change passed a full physical unplug/reconnect cycle in patched
QEMU with connector polling disabled: both transitions delivered USB IRQs and
bridge acknowledgment, DRM changed disconnected/connected, and the 128-byte EDID
returned. Force-connect remained clear after reconnection, native-format modesets,
and a driver reload. The verified configuration used active-low push-pull INT#;
bridge enable now explicitly selects that output mode after resets. Visual output
on the monitor was not reported in this test.

With USB interrupt URBs disabled in an isolated guest build, changing FL2000
polarity set both pending and HDMI-event flags in 0x8000; the next read cleared
them. Changing IT66121 output polarity did not set those flags across FL2000
active-high/active-low and debounce enabled/disabled combinations. Before correcting detection, both open-drain and push-pull configurations failed
the physical unplug test. Bit 27 of 0x8000
is documented as the HDMI-enable pin, not a raw INT# level; these results do not
prove a disconnected INT# wire. Physical hotplug delivery now works with the corrected configuration; the exact
board wiring and which FL2000 input reports the event were not measured. Diagnostic output controls exist only in temporary
test builds and are not included in the driver.

Interrupt setup explicitly enables EDID and external-monitor IRQs (0x803c bits
2/3) and clears bit 4, rather than relying on reset values. On the tested device,
bit 4 set keeps pending/HDMI-event flags latched across reads; bit 4 clear allows
the next read to acknowledge them and restores controlled IRQ delivery. This is
the opposite of the register manual's description of bit 4.
The HDMI control register layout follows the datasheet: interrupt enable is bit
19 and active-high polarity is bit 20 of 0x8064. Bridge enable restores the HPD
mask and active-low push-pull output after resets. Polling does not acknowledge HPD.

Stall recovery runs in a worker, serialized against stop, and gives up after
three retries. QEMU 11.1.2's USB descriptor parser stops on the FL2000's duplicate
bulk endpoint addresses and leaves interrupt endpoint 0x83 unconfigured, producing
immediate STALL responses. The local QEMU workaround below skips those duplicates
for USB ID 1d5c:2000 and allows endpoint 0x83 to be configured.

With QEMU/KVM and the matching kernel in `/boot`, run the hardware-free smoke test:

```sh
python3 tests/qemu_smoke.py 7.0.0-38-generic .
```

It requires a separately supplied base initramfs at `base-initramfs.cpio.gz`
in the repository root, appends an aligned cpio overlay, boots a disposable
guest and checks 100 load/unload cycles for both modules, alternating order.
Logs and guest images go to `/tmp/fl2000-qemu`. This checks module registration
and removal; USB probe, streaming, EDID and disconnect need the physical device.

For physical-device debugging, compile the guest KMS exerciser and pass the port:

```sh
cc -static -O2 -Wall -Wextra -Werror -I/usr/include/drm tests/kms_smoke.c -o /tmp/fl2000-kms-smoke
python3 tests/qemu_hardware.py --hostbus 2 --hostport 2 --kms-test /tmp/fl2000-kms-smoke
```

The hardware launcher preserves the base image's init script and appends only
modules plus the optional exerciser. It loads no host driver. The guest commands
are printed on launch. The exerciser displays colour bars in all three formats,
sends 120 dirty updates per format and reports process CPU time and optional
kernel function-profile results. USB-controller emulation affects these numbers.
The nspawn container must have USB device access for passthrough to attach.

For FL2000 port passthrough, QEMU 11.1.2 still aborts endpoint parsing on the
device's duplicate endpoints. Apply the local workaround to an extracted
QEMU 11.1.2 source tree; it preserves the first endpoint mapping and continues
parsing only for USB ID `1d5c:2000`:

```sh
patch -p1 < /workspace/patches/qemu-11.1.2-fl2000-duplicate-endpoints.patch
./configure --target-list=x86_64-softmmu --enable-kvm --enable-libusb --disable-docs --disable-tools --disable-guest-agent
ninja -C build qemu-system-x86_64
```

Select that executable with `tests/qemu_hardware.py --qemu /path/to/qemu/build/qemu-system-x86_64`.
The workaround does not modify the device's descriptors or replace the system
QEMU installation.

For more information check project's [Wiki](https://github.com/klogg/fl2000_drm/wiki)
