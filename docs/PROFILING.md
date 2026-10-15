# Profiling and measurement records

## Final matched measurements

The final matched run on the i3-6100T at 800x480 and 60 full-frame updates/s
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

To compare computation and PLL helpers without hardware:

```sh
python3 tests/benchmark_cpu.py
```

The userspace benchmark compares portable, SSSE3 and BMI2 XRGB8888 conversion
with tightly packed and padded native inputs, including an experimental native
SIMD candidate. It reports thread CPU time and an estimate for 60 updates per
second on one core. It excludes kernel FPU protection, mapping, USB, DMA and
scheduling costs; actual driver utilization needs measurements with the device
attached. Native-format savings apply when userspace supplies RGB888/RGB565;
fbdev still uses XRGB8888.

`python3 tests/benchmark_kernel.py --output /tmp/fl2000-kernel-bench` builds a
temporary benchmark module and loads it only inside hardware-free KVM. It
extracts the actual driver helpers, uses real FPU protection, checks prepared
bytes and measures computation with rotating cached buffers. Those buffers
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
