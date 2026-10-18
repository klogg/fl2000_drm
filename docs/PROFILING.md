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
