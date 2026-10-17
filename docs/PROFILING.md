# Profiling and measurement records

## Measured revision: f9e99ef

### Matched backend comparison

Portable, SSSE3, and BMI2 modules built from the same commit ran in one KVM
guest on the i3-6100T with Linux 7.0.0-38 and physical USB port passthrough.
The order was Portable / SSSE3 / BMI2, followed by BMI2 / SSSE3 / Portable.
Each case used 800x480 at 60 DIRTYFB updates/s and two 10-second untraced trials.
The tables report median update-process CPU as a percentage of one core.

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

SSSE3 substantially reduces full-frame XRGB8888 CPU relative to Portable;
BMI2 is close but slightly slower on this processor. RGB888 and RGB565 use
portable reordering in every build, so small differences between their columns
do not demonstrate SIMD native-format acceleration. Process CPU includes
userspace and synchronous DRM/ioctl work, excluding independent asynchronous
work and QEMU host CPU. The JSON preserves both trials and their observed ranges.
All six format suites, driver/dependency unloads, and guest shutdown completed
without kernel faults. This comparison is saved under
`matched_backend_comparison` in the
[current raw results](../tests/cpu_profile_current_results.json); detailed logs
are in `.qemu-build/performance-current/backend-comparison/`.

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
excludes independent asynchronous work and QEMU host execution. A static-screen
client consumed less than 0.003% of one core in each 10-second window; this does
not mean total static driver CPU is zero. QEMU host execution remained about
20-23% of one core during static scanout and 20-24% during full-frame updates,
compared with 11.69% before loading the driver. This accounting excludes host
USB IRQ work outside QEMU. Small damage reduces preparation work while the
adapter continues to receive full frames.

Separate diagnostic sampling attributed 83.7% of XRGB8888 kernel samples to
frame preparation, 76.2% for RGB888, and 66.7% for RGB565, with no lost samples.
Conversion and native wire reordering remain the largest sampled guest costs.
Function profiling measured `fl2000_streaming_update()` at 970, 482, and
317 microseconds per update respectively. These are inclusive elapsed times
from traced runs, not untraced CPU times; nested durations must not be summed.
No PLL calculations, bridge detection, EDID reads, or FL2000 register reads
were recorded inside the three profiled update windows.

Each five-second function-profile window recorded 330 successful frame URB
completions, or 66/s. The inferred payload rates are 76.032 MB/s for 24-bit
transport (XRGB8888 and RGB888 input) and 50.688 MB/s for RGB565. These estimates
exclude USB protocol overhead, come from traced windows, and do not measure
which frames were physically presented on the monitor.

The full-frame figures are close to the earlier SSSE3 results of
5.31% / 4.02% / 2.96%. Different measurement runs are not a matched A/B test;
the small differences do not establish a speedup from the recent cleanups.
The workload changes one source byte per update and declares a full-frame or
64x64 dirty rectangle. It excludes compositor rendering, GPU synchronization,
and application drawing costs, and does not predict higher-resolution results.

SSE/AVX preservation checks passed for all three formats. All workloads,
module/dependency unloads, and guest shutdown completed without kernel faults.
The [current raw results](../tests/cpu_profile_current_results.json) include
trial ranges, QEMU thread accounting, function profiles, sample counts, and
module hashes. Detailed console logs remain under
`.qemu-build/performance-current/run/`.

## Historical backend comparison

An earlier matched run on the i3-6100T at 800x480 and 60 full-frame updates/s
measured update-process CPU as follows (median of two 10-second trials):

| Input | Portable baseline | SSSE3 | BMI2 |
| --- | ---: | ---: | ---: |
| XRGB8888 | 10.84% | 5.31% | 5.54% |
| RGB888 | 4.02% | 4.02% | 4.02% |
| RGB565 | 2.94% | 2.96% | 2.92% |

SSSE3 remains the default x86-64 backend. BMI2 is selectable and needs no FPU
protection, but did not beat SSSE3 in the live XRGB8888 workload. An isolated
kernel benchmark found faster BMI2 RGB565 conversion (110 versus 152
microseconds for SSSE3 at 800x480); live RGB565 input needs no conversion, so
these are separate results. Kernel sampling attributed about 84% of XRGB8888
kernel samples to preparation and very few to FL2000 USB submission/completion.
QEMU host execution used roughly 20–24% of one core, with about 11% present
before driver loading. Host instruction sampling was unavailable because of
container permissions.

The isolated hot FPU begin/end loop added approximately 26 ns per iteration over
its timer/loop baseline; this excludes the full userspace state transition and
does not establish the cause of the earlier native SIMD regression. Native SIMD
was faster on cached benchmark buffers but did not improve the full USB
workload. Actual SSE/AVX state-preservation checks passed for all three
framebuffer formats. The long profiling run reached its watchdog after the last
measurement; the watchdog budget was corrected and a shorter run completed final
mode restoration, module unload and guest shutdown. Full data, sample counts and
methodology are in
[tests/cpu_profile_backend_results.json](../tests/cpu_profile_backend_results.json).
Final strict builds passed on Linux 7.0.0-34 and -38. In a physical-device KVM
test using the qemu64 CPU model with SSSE3 and BMI2 disabled, both optimized
modules successfully fell back to portable preparation for all three formats;
module unload and guest shutdown completed cleanly.

The guest profiler separates static-screen, full-frame, small-damage, and traced
workloads; `--quick` runs only the untraced full/small comparison. Build it
with:

```sh
cc -static -O2 -Wall -Wextra -Werror $(pkg-config --cflags libdrm) \
  tests/kms_profile.c -o /tmp/kms-profile
```

Pass it as the QEMU hardware launcher's `--kms-test /tmp/kms-profile` argument;
inside that guest it is named `/kms-smoke`. Run `/kms-smoke --baseline` before
loading the drivers, then mount debugfs and run `/kms-smoke` after loading them.
The workload selects 800x600 at 60 Hz when available, otherwise the first
advertised mode; it does not explicitly search for a preferred-mode flag. The
guest's `/proc/stat` totals were noisy in this VM; the saved process CPU and
function profiles are the primary comparison metrics.

The hardware profiling launcher uses the production bridge enumerator and
candidate configuration. It needs host `flock` and `i2ctransfer`; supply
`--i2ctransfer PATH` if the latter is not installed. The launcher appends the
enumerator, configuration, tools, and their shared libraries to the base image.
The minimal guest needs awk and built-in or preloaded `i2c-dev` support.

`tests/profile_qemu.py` compares supplied modules in one guest, reverses their
order on alternating trials, and records QEMU thread CPU alongside workload
windows. It subtracts `/proc` guest time from QEMU thread CPU to separate host
execution; this accounting has tick granularity and excludes host USB IRQ work
outside QEMU. `tests/kernel_sample.c` samples guest kernel instruction pointers
and call chains around the same workload markers. Sampling and function
profiling run separately from the untraced comparison, and their CPU figures are
diagnostic. The profiler accepts `--duration=N`, `--format=XRGB8888` (or
RGB888/RGB565), `--full-only`, `--static-only`, and `--check-fpu`. The last
option verifies SSE/AVX register preservation across actual framebuffer-update
syscalls on x86-64. Framebuffer mappings remain held from atomic preparation
until cleanup, including overlapping states that use the same framebuffer. PLL
searches run only when the mode changes. XRGB8888 conversion to RGB888 packs
four pixels into three dword writes; conversion to RGB565 packs pairs using
integer operations. Native input uses a 64-bit rotate-and-copy loop, omitting
row padding. Odd-width rows and partial wire words retain a scalar fallback.

To compare frame preparation computation without hardware:

```sh
python3 tests/benchmark_cpu.py
```

The userspace benchmark compares production portable, SSSE3 and BMI2 XRGB8888
conversion. Tightly packed and padded native inputs use the same portable
reordering on every backend, so they are measured once. PLL search and
abandoned SIMD experiments are excluded. It reports thread CPU time and an
estimate for 60 updates per second on one core. It excludes kernel FPU
protection, mapping, USB, DMA and scheduling costs; actual driver utilization
needs measurements with the device attached. Native-format savings apply when
userspace supplies RGB888/RGB565; fbdev still uses XRGB8888.

`python3 tests/benchmark_kernel.py --output /tmp/fl2000-kernel-bench` builds a
temporary benchmark module and loads it only inside hardware-free KVM. It
extracts the actual driver helpers, uses real FPU protection, checks prepared
bytes and measures computation with rotating cached buffers. It also measures
native reordering once through the production portable path. Those buffers
differ from the live GEM/USB buffers, so this benchmark cannot replace
end-to-end measurements. Framebuffer objects themselves use standard DRM shmem
GEM helpers; the five driver-owned wire buffers are USB transport allocations,
not a custom GEM type.

## Historical baselines

The following measurements predate the final backend results. In particular,
the first baseline did not yet benefit from partial damage preparation.
Different runs have different VM/host conditions; compare only matched trials.

QEMU/KVM profiling at 800x480
with 60 updates/s measured update-process CPU at approximately 4.0% of one core
for XRGB8888, 2.0% for RGB888, and 1.5% for RGB565. Updating a 64x64 rectangle
took essentially the same CPU as updating the full screen. These are process CPU
measurements, including DRM ioctl work, not total driver CPU; asynchronous work
and QEMU passthrough consume additional CPU. Function profiling measured frame
preparation at roughly 560/240/160 microseconds per update respectively. Nested
elapsed timings must not be summed or treated as CPU time. Raw results and
methodology are recorded in
[tests/cpu_profile_results.json](../tests/cpu_profile_results.json).

A matched post-restart run at 800x480 and 60 updates/s measured 64x64 update
process CPU falling from 9.83% to 1.49% for XRGB8888, 4.07% to 1.33% for RGB888,
and 2.93% to 1.24% for RGB565. Full-screen CPU stayed within about 3% of the
matched full-frame baseline. These figures use the same VM/host conditions and
must not be compared directly to the older absolute values above. Periodic
bridge/I2C reads were absent from all three optimized function profiles. See
[tests/cpu_profile_partial_results.json](../tests/cpu_profile_partial_results.json).

The optimized driver also passed a physical HDMI unplug/reconnect cycle in
patched QEMU with normal HPD-only connector detection. Both transitions followed
the USB interrupt worker through bridge acknowledgment and DRM detection, with
no periodic detect calls. Unplug disabled the CRTC; reconnect restored the
128-byte EDID and active 800x480 framebuffer console. Both modules unloaded
cleanly before guest shutdown. Visual output on the monitor was not
independently confirmed.
