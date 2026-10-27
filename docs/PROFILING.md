# Profiling

## Recorded measurements

The CPU comparisons below measure revision `f9e99ef`. Transport measurements
use `95a2a7c`. Keep revision and workload fixed when comparing results, and
repeat measurements after changing frame preparation or transport.

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
`matched_backend_comparison` in the [CPU results](../tests/cpu_profile_current_results.json).

### Default-backend profile

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
The [CPU results](../tests/cpu_profile_current_results.json) include trial
ranges, QEMU thread accounting, function profiles, sample counts and module
hashes.

## Transport diagnostics

Revision `95a2a7c` was measured with Linux 7.0.0-38, QEMU 11.1.2, one KVM
vCPU, physical USB port passthrough and a 64 MiB host USBFS allowance. The
production driver used its three-frame queue. The attached monitor advertises
800x480 at 66 Hz; synthetic 1080p60 timings measure transport throughput without
establishing visible image quality.

QEMU serialization limits whole-frame payloads followed by zero-length frame
delimiters to about 30 USB frames/s. Enabling the optional bulk OUT pipeline
raises delivery to about 60/s with the same driver and workload. See the
[QEMU setup](HARDWARE.md#bulk-transfer-queueing).

| Synthetic 1080p60 workload | Serialized QEMU frames/s | Pipelined frames/s |
| --- | ---: | ---: |
| RGB888 | 30.00 | 59.99 |
| RGB565 | 29.99 | 59.99 |

Each matched trial requested 60 full-frame DIRTYFB updates/s for 12 seconds.
Rates count successful whole-frame USB completions after trimming two seconds
from each end of the frame-size-matched interval. Function profiling and USB
tracing were enabled; these figures are transport diagnostics, not clean CPU
measurements.

A 30-second pipelined trial per format measured:

| Framebuffer format | USB frames/s | Payload MB/s |
| --- | ---: | ---: |
| XRGB8888 | 59.99 | 373.18 |
| RGB888 | 59.99 | 373.18 |
| RGB565 | 59.99 | 248.79 |

XRGB8888 includes conversion to RGB888 wire data. Removing the USB device during
active 1080p updates returned `ENODEV` to the client; reattachment restored both
wire formats and native-mode atomic checks. The successful guests completed
module unload and shutdown without kernel warnings, oopses or panics.

Injecting `LIBUSB_ERROR_NO_MEM` on the third RGB888 payload exercised the QEMU
submission-error fallback. The endpoint continued in serialized mode at about
30 USB frames/s and completed subsequent format changes and module unload.
The patch frees failed requests and resets the copied-byte count before retry;
endpoint reconfiguration permits pipelining again.

LBUF overflow recurs with either QEMU queueing mode, including at 60 USB
frames/s. Status sampling through the interrupt worker recorded the same 66
samples in each mode: 59 with overflow, one with overflow and VGA error, and
six with underflow. None reported LBUF halt. The driver clears LBUF flags with
`0x8004[12]`; clearing them is not fault recovery. These sticky flags do not
count lost frames or establish visible corruption, and do not explain the
measured throughput difference.

The [transport results](../tests/highres_transport_results.json) preserve
module/emulator hashes, completion intervals, status samples, sustained runs
and injected failures, including unsuccessful emulator prototypes. Repeat with
a native host or dedicated USB controller and competing traffic before drawing
conclusions about physical presentation or device-wide limits.

## FIFO water-level observations

The [register experiment](../tests/fifo_watermark_results.json) used revision
`95a2a7c`, 800x480 RGB888 and the physical adapter through QEMU/KVM. Each trial
reset application logic, restored the mode and cleared status before sending
an isolated burst. Status and water levels were sampled before its terminating
zero-length packet.

| Burst | Peak at `0x8060[23:8]` | Overflow |
| --- | --- | --- |
| 96 KiB | `0x5ffe` | No |
| 128 KiB | `0x7ffe` | No |
| 129 KiB | `0x7ffe` or zero | Yes |

Below overflow, peak values follow bytes divided by four minus two. This is
consistent with DWORD units, but neither the units nor the two-word offset is
explained by the register map. The result is a burst-size boundary under the
tested reset and passthrough conditions, not proof of physical FIFO capacity,
pixel loss or a required USB transfer-size limit.

Changing `0x8044[31:16]` to 1, 10240, 16384, 32767 or 65535 and testing both
settings of `0x803c[11]` did not move the boundary. Writes of 24576/16384 to
`0x8034/0x8038` were also tested with each bit-11 setting; with bit 11 clear,
the ready threshold was additionally raised to 32767. These 60 trials had
three repetitions per configuration and burst size. Accumulation and bit-11
writes read back correctly; no bulk transfer failed.

Another 54 trials alternated accumulation values `0x2800` and `0x2400` across
nine burst sizes from 36 to 129 KiB. Peaks below overflow were identical, and
both settings asserted overflow at 129 KiB in every repetition. First-ITP
levels from `0x8048[31:16]` varied and exceeded `0x2400`. The accumulation
field did not directly cap either observation under these conditions.

The manual describes `0x8034/0x8038` as high/low water levels without defining
them as thresholds or flow controls. RW access alone does not establish their
purpose. Reads of either address returned the preceding valid register value
on this adapter, preventing trustworthy measurements or write verification.
Zero was written after testing; restoration of these registers is unverified.
Original accumulation, PPE and ready settings were restored, and driver reload,
atomic display and native-format checks passed.

The manual explicitly defines `0x8030[14:0]` as a watermark for asserting
ready and `0x8060[23:8]` as the last frame's highest water level. Bit 11 is
ambiguous: its description says PPE block enable while its signal name is
`cfg_ppe_block_dis`. No watermark enable sequence or backpressure relationship
is documented. QEMU timing effects and undocumented dependencies remain
unresolved; no production register programming was changed.

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

## Measurement records

Use matched trials within each dataset; different revisions and VM/host
conditions do not provide comparable absolute CPU percentages.

- [CPU backend and damage measurements](../tests/cpu_profile_current_results.json)
- [High-resolution transport](../tests/highres_transport_results.json)
- [FIFO water levels](../tests/fifo_watermark_results.json)
- [USB teardown and recovery](../tests/teardown_results.json)

Additional datasets retain source hashes and experimental conditions:

- [Backend computation](../tests/cpu_profile_backend_results.json)
- [Partial damage](../tests/cpu_profile_partial_results.json)
- [CPU baseline](../tests/cpu_profile_results.json)
- [Transport profiling](../tests/transport_profile_results.json)
- [Status clearing and QEMU queues](../tests/transport_investigation_results.json)
