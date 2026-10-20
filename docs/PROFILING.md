# Profiling

## Recorded measurements

The latest published comparison measures revision `f9e99ef`; it is not a
measurement of the current checkout. Rerun the procedure below after changing
frame preparation or transport.

### Matched backend comparison

Portable, SSSE3, and BMI2 modules built from the same commit ran in one KVM
guest on the i3-6100T with Linux 7.0.0-38 and physical USB port passthrough.
The order was Portable / SSSE3 / BMI2, followed by BMI2 / SSSE3 / Portable.
Each case used 800x480 at 60 DIRTYFB updates/s and two 10-second untraced
trials. The tables report median update-process CPU as a percentage of one
core.

Full-frame updates:

| Input | Portable | SSSE3 | BMI2 |
| --- | ---: | ---: | ---: |
| XRGB8888 | 10.65% | 5.32% | 5.46% |
| RGB888 | 3.83% | 3.95% | 3.96% |
| RGB565 | 2.92% | 2.79% | 2.87% |

64x64 damage updates:

| Input | Portable | SSSE3 | BMI2 |
| --- | ---: | ---: | ---: |
| XRGB8888 | 1.39% | 1.40% | 1.30% |
| RGB888 | 1.20% | 1.20% | 1.27% |
| RGB565 | 1.14% | 1.03% | 1.09% |

SSSE3 substantially reduces full-frame XRGB8888 CPU relative to Portable; BMI2
is close but slightly slower on this processor. RGB888 and RGB565 use portable
reordering in every build, so small differences between their columns do not
demonstrate SIMD native-format acceleration. Process CPU includes userspace
and synchronous DRM/ioctl work, excluding independent asynchronous work and
QEMU host CPU. The JSON preserves both trials and their observed ranges. All
six format suites, driver/dependency unloads, and guest shutdown completed
without kernel faults. This comparison is saved under
`matched_backend_comparison` in the [current raw
results](../tests/cpu_profile_current_results.json); detailed logs are in
`.qemu-build/performance-current/backend-comparison/`.

### Separate default-backend diagnostics

Measured on the Intel Core i3-6100T using Linux 7.0.0-38, patched QEMU 11.1.2,
one KVM vCPU, physical USB port passthrough, and the default SSSE3 backend.
The display mode was 800x480. Each full-frame and 64x64-damage case ran three
10-second untraced trials at 60 updates/s; values below are medians and refer
to one CPU core.

| Input | Full-frame process CPU | 64x64 damage CPU | CPU reduction |
| --- | ---: | ---: | ---: |
| XRGB8888 | 5.24% | 1.30% | 75% |
| RGB888 | 3.97% | 1.21% | 70% |
| RGB565 | 2.82% | 1.11% | 61% |

Process CPU includes benchmark userspace and synchronous DRM/ioctl work. It
excludes independent asynchronous work and QEMU host execution. A
static-screen client consumed less than 0.003% of one core in each 10-second
window; this does not mean total static driver CPU is zero. QEMU host
execution remained about 20-23% of one core during static scanout and 20-24%
during full-frame updates, compared with 11.69% before loading the driver.
This accounting excludes host USB IRQ work outside QEMU. Small damage reduces
preparation work while the adapter continues to receive full frames.

Separate diagnostic sampling attributed 83.7% of XRGB8888 kernel samples to
frame preparation, 76.2% for RGB888, and 66.7% for RGB565, with no lost
samples. Conversion and native wire reordering remain the largest sampled
guest costs. Function profiling measured `fl2000_streaming_update()` at 970,
482, and 317 microseconds per update respectively. These are inclusive elapsed
times from traced runs, not untraced CPU times; nested durations must not be
summed. No PLL calculations, bridge detection, EDID reads, or FL2000 register
reads were recorded inside the three profiled update windows.

Each five-second function-profile window recorded 330 successful frame URB
completions, or 66/s. The inferred payload rates are 76.032 MB/s for 24-bit
transport (XRGB8888 and RGB888 input) and 50.688 MB/s for RGB565. These
estimates exclude USB protocol overhead, come from traced windows, and do not
measure which frames were physically presented on the monitor.

The workload changes one source byte per update and declares a full-frame or
64x64 dirty rectangle. It excludes compositor rendering, GPU synchronization,
and application drawing costs, and does not predict higher-resolution results.

SSE/AVX preservation checks passed for all three formats. All workloads,
module/dependency unloads, and guest shutdown completed without kernel faults.
The [current raw results](../tests/cpu_profile_current_results.json) include
trial ranges, QEMU thread accounting, function profiles, sample counts, and
module hashes. Detailed console logs remain under
`.qemu-build/performance-current/run/`.

## Transport diagnostics

On 2026-10-19, the ordered format-programming change on top of `98c7b18`
was tested with Linux 7.0.0-38, patched QEMU 11.1.2, one KVM vCPU and physical
USB port passthrough. The attached display advertises only 800x480 at 66 Hz.
The 1080p60 mode was synthetic and establishes no monitor image-fidelity result.

Each format ran 20 seconds of full-frame DIRTYFB updates requested at 60/s.
USB tracing measured successful whole-frame completions, excluding two seconds
at each end of the longest run with the expected frame size. These traced
results are transport diagnostics, not clean CPU benchmarks.

| Mode | Wire format | USB frames/s | Payload MB/s |
| --- | --- | ---: | ---: |
| 800x480 | RGB888 | 65.49 | 75.45 |
| 800x480 | RGB565 | 65.68 | 50.44 |
| Synthetic 1080p60 | RGB888 | 29.99 | 186.59 |
| Synthetic 1080p60 | RGB565 | 30.02 | 124.51 |

Two separate experimental builds sampled status through the existing interrupt
worker once per second. One additionally cleared detection bit 30 on every
outgoing I2C-control register write, including initialization; USB traces
confirmed the difference. Both retained approximately 30 USB frames/s at
1080p, while the controller frame counter advanced at approximately 60/s.
A busy loop on the sole guest vCPU did not materially change that result.
Neither experimental change is part of the production driver.

All steady-state samples retained the LBUF overflow bit; none had underflow
set. These initial tests could not distinguish stale flags from new events or
establish full-rate 1080p throughput. The follow-up below resolves the clear
control and identifies a QEMU queueing limitation.

The [transport results](../tests/transport_profile_results.json) record source
and module hashes, intervals, status observations, experiment conditions and
remaining tests. Raw USB traces, guest logs and temporary experiment patches
are retained locally under `.qemu-build/adcds-tests/`.

### Follow-up: LBUF status and QEMU queues

A controlled comparison found that the dedicated `0x8004` bit-12 strobe clears
LBUF flags. Writing status bit 31, as the status table suggests, does not clear
the observed overflow flag on this adapter. With scanout blanked, the bit-12
variant clears overflow and subsequently records underflow; both comparison
variants retain stale overflow. The production driver now uses the dedicated
strobe without writing the read-to-clear event register.

Overflow reappears during active streaming at both native resolution and
1080p, including runs that deliver 60 USB frames/s. It is not sufficient to
explain the 30-frame result, and clearing flags is not LBUF fault recovery.

Host transfer traces showed that normal QEMU submits one bulk request at a
time, alternating a whole-frame payload and its zero-length packet. An
experimental QEMU enabled bulk-OUT pipelining specifically for this adapter.
The matched comparison used the same two-frame test driver in both emulators:

| Synthetic 1080p60 workload | Normal QEMU frames/s | Pipelined QEMU frames/s |
| --- | ---: | ---: |
| RGB888 | 29.99 | 59.99 |
| RGB565 | 30.00 | 59.99 |
| RGB888 with guest CPU load | 30.00 | 59.99 |

Pipelined payload throughput reached 373.20 MB/s for RGB888 and 248.80 MB/s
for RGB565. The controller counter remained near 60/s in both configurations.
This identifies emulator serialization as the cause of the measured rate
halving in this setup; no driver pacing or pixel-processing change was needed.
Image fidelity, bandwidth margin with competing USB traffic and other hardware
remain unvalidated.

The experimental QEMU is not a deployable fix: its three-frame RGB888 run hit
an assertion after a host submission failure. Three payloads need 17.8 MiB,
exceeding the host's 16 MiB USBFS allowance; the precise libusb failure code
was not recorded. The matched experiment used two frames to stay below that
limit. Production retains three frames. The normal QEMU executable, source
and build object were restored; robust pipelining and error handling require
separate QEMU work.

The investigation also exposed an allocation warning when a 1080p RGB888
buffer required a contiguous order beyond the direct-DMA allocator's limit.
The optional DMA attempt now uses `__GFP_NOWARN` and retains its existing page
fallback, allowing IOMMU-backed allocations to keep using the DMA path.

The [follow-up results](../tests/transport_investigation_results.json) contain
the status matrix, matched trials, hashes and validation outcomes. Detailed
logs, traces and temporary experiment patches remain under
`.qemu-build/transport-investigation/`.

The removal-time `Frame conversion failed: -2` recorded in these measurements
was subsequently traced to USB endpoint disable preceding DRM quiescence.
The [teardown follow-up](../tests/teardown_results.json) verifies ordered DRM
and USB cleanup with default hard unbind, including removal during active
framebuffer updates.

## Measuring the driver

Measure backends in the same guest and alternate their order across trials.
Keep mode, update rate, CPU model and duration fixed. Record the source
revision, module hashes and kernel configuration. SSSE3 is the default x86-64
backend; BMI2 avoids FPU protection but requires measurement on the target
processor.

The guest profiler separates static-screen, full-frame, small-damage, and
traced workloads; `--quick` runs only the untraced full/small comparison.
Build it with:

```sh
cc -static -O2 -Wall -Wextra -Werror $(pkg-config --cflags libdrm) \
  tests/kms_profile.c -o /tmp/kms-profile
```

Pass it as the QEMU hardware launcher's `--kms-test /tmp/kms-profile`
argument; inside that guest it is named `/kms-smoke`. Run `/kms-smoke
--baseline` before loading the drivers, then mount debugfs and run
`/kms-smoke` after loading them. The workload selects 800x600 at 60 Hz when
available, otherwise the first advertised mode; it does not explicitly search
for a preferred-mode flag. The guest's `/proc/stat` totals were noisy in this
VM; the saved process CPU and function profiles are the primary comparison
metrics.

The hardware profiling launcher uses the production bridge enumerator and
candidate configuration. It needs host `flock` and `i2ctransfer`; supply
`--i2ctransfer PATH` if the latter is not installed. The launcher appends the
enumerator, configuration, tools, and their shared libraries to the base
image. The minimal guest needs awk and built-in or preloaded `i2c-dev`
support.

`tests/profile_qemu.py` compares supplied modules in one guest, reverses their
order on alternating trials, and records QEMU thread CPU alongside workload
windows. It subtracts `/proc` guest time from QEMU thread CPU to separate host
execution; this accounting has tick granularity and excludes host USB IRQ work
outside QEMU. `tests/kernel_sample.c` samples guest kernel instruction
pointers and call chains around the same workload markers. Sampling and
function profiling run separately from the untraced comparison, and their CPU
figures are diagnostic. The profiler accepts `--duration=N`,
`--format=XRGB8888` (or RGB888/RGB565), `--full-only`, `--static-only`, and
`--check-fpu`. The last option verifies SSE/AVX register preservation across
actual framebuffer-update syscalls on x86-64.

To compare frame preparation computation without hardware:

```sh
python3 tests/benchmark_cpu.py
```

The userspace benchmark compares production portable, SSSE3 and BMI2 XRGB8888
conversion. Tightly packed and padded native inputs use the same portable
reordering on every backend, so they are measured once. It reports thread CPU
time and an estimate for 60 updates per second on one core. It excludes kernel
FPU protection, mapping, USB, DMA and scheduling costs; actual driver
utilization needs measurements with the device attached. Native-format savings
apply when userspace supplies RGB888/RGB565; fbdev still uses XRGB8888.

`python3 tests/benchmark_kernel.py --output /tmp/fl2000-kernel-bench` builds a
temporary benchmark module and loads it only inside hardware-free KVM. It
extracts the actual driver helpers, uses real FPU protection, checks prepared
bytes and measures computation with rotating cached buffers. It also measures
native reordering once through the production portable path. Those buffers
differ from the live GEM/USB buffers, so this benchmark cannot replace
end-to-end measurements. Framebuffer objects themselves use standard DRM shmem
GEM helpers; the five driver-owned wire buffers are USB transport allocations,
not a custom GEM type.

## Raw data

The [published comparison](../tests/cpu_profile_current_results.json) contains
trial ranges, sampling/function profiles, module hashes and QEMU accounting.
Earlier records remain available for reproducibility:

- [Backend experiments](../tests/cpu_profile_backend_results.json)
- [Partial damage comparison](../tests/cpu_profile_partial_results.json)
- [Initial baseline](../tests/cpu_profile_results.json)

These records describe different revisions and VM/host conditions. Compare
only matched trials; older absolute percentages are not current performance
claims.
