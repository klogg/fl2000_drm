# Development and validation

## Build and source checks

Build against matching Linux 7.0 headers with warnings treated as errors:

```sh
make KVER=7.0.0-38-generic W=1 KCFLAGS=-Werror
```

Repeat for `FL2000_FRAME_BACKEND=generic`, `ssse3` and `bmi2` on a supported
x86-64 target. In-tree and other-architecture validation are separate tasks.
See [build instructions](../README.md#build) for missing DRM helpers.

Use clang-format 18 and the checked-in `.clang-format`:

```sh
scripts/clang-format.sh --dry-run
scripts/checkpatch.sh
shellcheck scripts/*.sh userspace/*.sh
ruff check tests
pymarkdown scan README.md docs/*.md
```

The C scripts cover production sources and headers, including bridge, backend
and compatibility objects. Checkpatch permits `NEW_TYPEDEFS` only for GCC vector
aliases in `frame/ssse3.c`, and `CAMELCASE` only for kernel USB descriptor fields
in the USB driver and interrupt source. Standalone test C utilities follow the
same formatting but are not included in those scripts.

Kernel callback types must match their framework declarations. Sonar exclusions
live in `.github/workflows/sonarcloud.yaml`, not source annotations. Its
file-scoped `c:S995` exceptions also affect ordinary functions, so review their
const correctness manually. `tests/compile_commands.py` merges default and BMI2
build databases; it does not analyze every shared-source configuration.

## Source API documentation

Use Linux kernel-doc syntax for all production functions and important types,
operation tables, ownership and locking contracts. Document parameters,
sleeping/locking context and return values at the definition; shared types and
backend interfaces are documented in headers.
Keep every code contract in source, with concise comments for trivial helpers.
Markdown adds setup, overall design and validation evidence; sources must not
refer back to Markdown for required documentation.

With a full Linux 7.0 source tree, check comments and generate reStructuredText:

```sh
kernel_src=/path/to/linux-7.0
python3 "$kernel_src/tools/docs/kernel-doc" -Wall -Werror -none \
  fl2000.h fl2000_frame.h fl2000_registers.h fl2000_*.c \
  bridge/*.[ch] frame/*.[ch]
python3 "$kernel_src/tools/docs/kernel-doc" -Wall -Werror -rst \
  fl2000.h fl2000_frame.h fl2000_registers.h fl2000_*.c \
  bridge/*.[ch] frame/*.[ch] \
  > /tmp/fl2000-api.rst
```

The checker needs its matching `tools/lib/python/kdoc` package; distribution
headers may omit it. In-tree `W=1` builds check C comments; external-module
builds do not run that check. Validate headers and external sources explicitly
as above. `tests/test_tools.py` checks function/type/table coverage and forbids
source references to Markdown. The generated reference can be included in
kernel Sphinx documentation; generated output is not tracked here. Use the
[kernel-doc format][kernel-doc] rather than Doxygen commands such as `@param`.

[kernel-doc]: https://docs.kernel.org/doc-guide/kernel-doc.html

## Hardware-free regressions

Run all sanitizer and script harnesses with Python 3 and an ASan/UBSan-capable
C compiler:

```sh
for test in logic register_locking registers edid bridge bridge_probe \
    driver drm interrupt tools enumerator; do
    UBSAN_OPTIONS=halt_on_error=1 python3 "tests/test_$test.py" || exit 1
done
```

`test_logic.py` exercises portable, SSSE3 and BMI2 conversion with independent
byte references. `test_registers.py` includes forced big-endian scalar storage.
The enumerator harness uses synthetic sysfs and tool responses, including
built-in drivers, deferred clients and explicit reprobe.
`tests/source_helpers.py` extracts production functions and structures; these
harnesses model kernel interfaces rather than emulate a complete kernel or USB
controller. They do not replace lockdep, KASAN, KCSAN or hardware tests.

The hardware-free QEMU smoke test checks 100 alternating module load/unload
cycles. See [QEMU setup](../README.md#isolated-qemu-testing). Frame computation
benchmarks and live profiling are described in [profiling](PROFILING.md).

## Hardware guest tests

Use the prepared initramfs and USB port passthrough; load modules only in the
guest. `tests/qemu_utils.py` checks module versions, copies installed dependency
modules and appends a newc overlay without decompressing the base image.
`/fl2000-startup` loads dependencies, bridge candidates and FL2000, then runs the
production enumerator. The minimal guest does not test udev/systemd activation.

Build `tests/kms_smoke.c` and `tests/usb_reset.c` as static executables for the
minimal guest. The KMS test checks XRGB8888, RGB888 and RGB565, invalid geometry,
nonblocking plane and CRTC-only commits, flip events and dirty-framebuffer
updates. The image combines color bars with single-pixel RGB detail. Options
are `--list-modes`, `--mode INDEX` and `--hold`. The default prefers 800x600 at
60 Hz when advertised, otherwise the first mode. `--stream FORMAT SECONDS`
runs one format at 60 requested full-frame updates/s for 1–600 seconds,
without the atomic validation suite. Formats are `XRGB8888`, `RGB888` and
`RGB565`. `--test-1080p FORMAT SECONDS` runs the same workload with synthetic
1920x1080 at 60 Hz timings, even when the monitor does not advertise that mode;
use it for transport stress, not image-fidelity validation. `--hold`
keeps an open DRM file until `/tmp/release-hold` exists, then expects `ENODEV`
after unbind/removal; remove any old marker before repeating it. KMS completion
does not establish physical presentation.

Build `tests/lifecycle_fault.c` as a separate external module against the guest
kernel, with `obj-m := lifecycle_fault.o` in a temporary Makefile. It needs
kprobes and rejects non-QEMU DMI systems. Include helpers in the overlay:

```sh
python3 tests/qemu_hardware.py --output .qemu-build/hw \
  --kms-test PATH/kms-smoke --guest-file PATH/usb-reset \
  --guest-file PATH/lifecycle_fault.ko --i2ctransfer PATH/i2ctransfer
```

`--guest-file` copies each file to the guest root under its basename, preserving
its mode. Keep output paths short enough for QEMU's Unix-domain socket. Run
`/fl2000-startup`, mount debugfs, then run `/kms-smoke`. Useful lifecycle checks
include bridge unbind/rebind with an open DRM file, USB interface unbind/rebind,
USB reset and QMP removal/addition. Resolve device names from current guest
sysfs; host bus/device numbers are not guest numbers.

For status and IRQ-acknowledgment injection, load `lifecycle_fault.ko` with
`mode=status` or `mode=i2c`, `failures=3`, then unbind/rebind the bridge to
restart interrupt processing. Allow at least ten seconds and inspect `injected`,
`polling_on` and `polling_off` under `/sys/module/lifecycle_fault/parameters/`.
These counters show flag transitions; inspect DRM worker execution separately
to establish that polling actually ran. The DRM regression checks that fallback
calls `drm_kms_helper_poll_reschedule()` to queue work.

For bulk recovery, disable fbcon cursor blinking and load the injector with
`mode=bulk failures=1` while a static image streams. Require `injected=1`, a
recovery, sustained `bulk_completions` and zero `frame_updates`. This checks
recovery without a new framebuffer update. The tested run recovered once and
recorded 193 successful completions in three seconds. Injection modifies
software results, not physical USB behavior. The injector assumes one adapter;
its I2C mode targets the supplied IT66121 IRQ handler. Unload it before either
production module.

## USB capture

Supply guest usbmon, capture tools and their dependencies in the base initramfs;
the driver overlay does not include them. Load usbmon only in the guest. Start
without attaching the device:

```sh
scripts/emulate.sh --no-device --hostbus 2 --hostport 2
```

Start a guest capture on `usbmon0`, then use Ctrl-a c to enter the QEMU monitor:

```text
device_add usb-host,id=fl2000,bus=xhci.0,hostbus=2,hostport=2
```

Return with Ctrl-a c and run `/fl2000-startup`. Capture USB enumeration so the
dissector learns product ID `1d5c:2000`. Replay the exported capture as a regular
user with Lua-enabled TShark:

```sh
tshark -X lua_script:scripts/fl2k.lua -r capture.pcapng -Y fl2k
tshark -X lua_script:tests/fl2k_headless.lua \
  -X lua_script1:scripts/fl2k.lua -r capture.pcapng -q
```

Require `HEADLESS_GUI_CALLBACKS_PASS`; Lua errors can coexist with a zero TShark
exit status. The callback test substitutes window widgets but uses real packet
decoding and listeners. Test visual layout separately in Wireshark.
`python3 tests/test_fl2k.py` generates hardware-free capture fixtures.

## Validation scope

External x86-64 builds have used Ubuntu 7.0.0-34 and 7.0.0-38 headers. Physical
passthrough testing used patched QEMU 11.1.2, KVM and one HDMI display advertising
800x480 at 66 Hz, with a 32 MHz pixel clock. Covered paths include all formats
and backends, physical HDMI unplug/reconnect, open-file lifetime and bridge
rebind, USB interface cycles/reset, QMP removal/addition and software fault
injection. These are targeted observations, not a claim that every revision or
hardware configuration passed. SSE/AVX preservation and runtime CPU fallback
have also been tested in guests.

Local build/test logs are ignored under `.qemu-build/`. The latest maintainer
fix run records its revisions and checks in `maintainer-fixes/results.json`;
its guest logs are under `mf/`. Earlier lifecycle evidence is under
`lifecycle-validation/`, `lv/` and `pm/`. These artifacts are not distributed.
Published CPU measurements remain in the tracked JSON files linked from
[profiling](PROFILING.md).

## Validation backlog

- **System PM:** `pm_test=devices` hangs in the emulated xHCI resume path even
  with both drivers unloaded; the freezer-only control passed. Diagnose that
  environment, then test S3, s2idle, reset-resume, repeated active/idle cycles
  and unplug during suspend on a physical host or dedicated USB controller.
- **Physical faults and allocation failures:** test power loss, real stalls,
  short transfers, failed submissions, retry exhaustion and memory pressure
  across USB controllers and hubs. Require bounded bulk retries, correct DMA
  ownership, polling fallback and clean reconnect with open DRM files.
- **Native bridge users:** validate IT6610/21/22 OF boards, regulators, reset
  GPIO, IRQ wiring, bridge chaining, audio and probe deferral. FL2000 testing
  cannot establish compatibility for existing upstream users.
- **Multiple adapters:** simultaneous enumeration and scanout, mixed bridges,
  independent IRQ/reset and removal of one adapter while another streams.
- **Modes and fidelity:** additional resolutions, clocks and displays; verify
  channel order, quantization, pitch, crops, damage edges and modeset transitions
  with visible output or capture. Flip events and byte tests are insufficient.
- **High-resolution transport:** the matched QEMU experiment isolated a
  serialization limit: bulk-OUT pipelining raised synthetic 1080p60 delivery
  from 30 to 60 USB frames/s in both formats. A deployable QEMU change still
  needs submission-error handling and adequate USBFS memory. Verify images with
  a suitable display/capture and repeat on a dedicated controller or physical
  host, including competing USB traffic. LBUF clearing is verified; the cause
  and consequences of overflow reappearing during active streaming still need
  investigation. See the [transport measurements](PROFILING.md#transport-diagnostics).
- **Kernel integration:** full built-in/modular kernel builds, helper selection,
  non-x86 architectures, endian/alignment and DMA constraints. External x86
  header builds do not cover this.
- **Concurrency and endurance:** sustained modesets, EDID, damage, reset and
  teardown under lockdep, KASAN and KCSAN. Check reference/memory growth and
  work/URB drainage. Investigate observed `Frame conversion failed: -2`
  teardown diagnostics rather than treating successful rebind as resolution.
- **Distribution services:** packaged udev/systemd ordering, boot-time devices,
  late module loading, registered built-in bridges, failed probe retries and
  cancellation during removal. Direct enumeration in a minimal guest does not
  exercise service management.

## Remaining implementation work

Upstream submission needs patches against the existing `ite-it66121.c` and
its Kconfig, rather than a second implementation with the same module name.
Review the local adapter-property and component-bind contracts as part of that
integration.

Native VGA requires output selection, VGA connector/EDID handling, monitor
interrupts and analog validation. LBUF halt and VGA error recovery are not
implemented; current code only acknowledges overflow/underflow flags. HDMI audio
transport on FL2000 and color management need implementation. Resolve the AVI
packet register-layout conflict documented in [the register reference](REGISTERS.md)
before changing shared bridge programming.

Compression remains deferred. Any future implementation must be optional and
measure bandwidth savings, CPU cost and image quality against uncompressed
transport.
