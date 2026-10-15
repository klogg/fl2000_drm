# FL2000 USB display driver

[![Build](https://github.com/klogg/fl2000_drm/actions/workflows/makefile.yml/badge.svg)](https://github.com/klogg/fl2000_drm/actions/workflows/makefile.yml)
[![Stylecheck](https://github.com/klogg/fl2000_drm/actions/workflows/codingstyle.yaml/badge.svg)](https://github.com/klogg/fl2000_drm/actions/workflows/codingstyle.yaml)
[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=klogg_fl2000_drm&metric=alert_status)](https://sonarcloud.io/dashboard?id=klogg_fl2000_drm)

An out-of-tree DRM/KMS driver for Fresco Logic FL2000 USB display adapters
(USB ID `1d5c:2000`) with an ITE IT66121 HDMI bridge. The bridge core derives
from Linux v7.0, with adaptations for FL2000 I2C, reset, and USB lifetime.

This development revision targets **Linux 7.0**. Strict x86-64 builds and
hardware tests used Ubuntu kernels 7.0.0-34 and 7.0.0-38. Older kernels need an
API port. Hardware validation used one 800x480 HDMI display in patched QEMU;
visual output was not independently confirmed.

## Build

Install a C compiler, Make, and headers matching the target Linux 7.0 kernel.
Build against the running kernel, or select an installed header tree:

```sh
make
make KSRC=/usr/src/linux-headers-7.0.0-38-generic
```

The resulting modules are `fl2000.ko` and `it66121.ko`. Use this repository's
adapted bridge module; the stock upstream module is not interchangeable.
The driver creates its own bridge client at I2C address 0x4c or 0x4d, so an
`i2c_bus_num` parameter is not needed.

`FL2000_FRAME_BACKEND=auto` selects SSSE3 for x86-64 target kernels and portable
code elsewhere. Explicit choices are `ssse3`, `bmi2`, and `generic`. Optimized
backends fall back to portable processing when CPU features are unavailable.

```sh
make FL2000_FRAME_BACKEND=generic
```

If module signing is needed, `scripts/sign.sh` signs both modules. Run
`scripts/sign.sh --help` for signing-key and tool overrides. Signing does not
by itself enroll the certificate in the target kernel's trust store.

## Isolated QEMU testing

Tests load modules only inside a marked guest. Requirements are QEMU with KVM
and USB-host support, access to `/dev/kvm` and the selected USB port, and the
matching kernel image in `/boot`. Containers must expose those device nodes.
The tested QEMU 11.1.2 needs the included
[duplicate-endpoint workaround](docs/HARDWARE.md#qemu-1112-workaround).

Supply your own `base-initramfs.cpio.gz`; it is **not included**. Its init must
provide a root shell, mounted `/proc`, `/sys`, and `/dev`, and module tools.
DRM, shmem, I2C, component, and USB host-controller dependencies must already
be built into the guest kernel or loaded by the base init. The unattended
smoke test and kernel benchmark replace `/init`, so their dependencies must
be built into the kernel. Merely including module files is insufficient:
`/fl2000-startup` uses `insmod`.

Set `BASE_INITRAMFS` for a different base image. The launchers append an aligned
cpio overlay without decompressing the base. They share `--qemu`, `--cpu`,
`--bios-path`, and `--base` options, and prefer the local patched QEMU build
when present. `QEMU` can select another executable. Firmware is inferred for
source builds, or selected explicitly with `--bios-path`.

Run the hardware-free 100-cycle module load/unload test:

```sh
python3 tests/qemu_smoke.py 7.0.0-38-generic .
```

For physical-device testing, substitute the adapter's actual USB bus and port:

```sh
scripts/emulate.sh --hostbus 2 --hostport 2
```

Inside the guest:

```sh
/fl2000-startup
```

Startup loads IT66121 before FL2000 and is safe to repeat. No host USB
controller is unbound. Logs default to `.qemu-build/fl2000-tests`. Use
`KVER`, `MODULE_DIR`, or `QEMU_OUTPUT_DIR` to override the wrapper's defaults.

The guest colour-bar exerciser needs libdrm development headers, pkg-config,
and static libc development files when built for the minimal initramfs:

```sh
cc -static -O2 -Wall -Wextra -Werror $(pkg-config --cflags libdrm) \
  tests/kms_smoke.c -o /tmp/fl2000-kms-smoke
scripts/emulate.sh --hostbus 2 --hostport 2 \
  --kms-test /tmp/fl2000-kms-smoke
```

After `/fl2000-startup`, run `/kms-smoke` in that guest. To finish:

```sh
rmmod fl2000
rmmod it66121
/usr/lib/klibc/bin/poweroff -f
```

## Supported behavior and limits

- Linear XRGB8888, RGB888, and RGB565 framebuffers, including pitch and crops.
- Per-buffer damage tracking, persistent USB transport buffers, and standard
  DRM shmem GEM objects. There is no custom GEM buffer type.
- USB hotplug interrupts with connector polling as an error fallback.
- Bandwidth-based RGB888/RGB565 selection for XRGB8888 input. Native formats
  retain their representation; modes exceeding their bandwidth are rejected.
- Software flip completion, without a physical-vblank or presentation guarantee.

Compression is deferred because host-side encoding adds CPU work. Future
support will be optional; uncompressed transport remains the default. Its
purpose would be to reduce USB bandwidth when that trade-off is useful.

Interlaced, double-scan, stereoscopic, and fractional source rectangles are
rejected. Suspend/resume, multiple dongles, extensive mode coverage, and error
recovery still need broader hardware validation. LBUF halt and VGA error
recovery are not implemented. See the
[remaining work](docs/DEVELOPMENT.md#remaining-work).

## Performance

A matched i3-6100T QEMU/KVM run at 800x480 with 60 full-frame updates/s measured
the following update-process CPU, as a percentage of one core. Values are
medians of two 10-second trials:

| Input | Portable | SSSE3 | BMI2 |
| --- | ---: | ---: | ---: |
| XRGB8888 | 10.84% | 5.31% | 5.54% |
| RGB888 | 4.02% | 4.02% | 4.02% |
| RGB565 | 2.94% | 2.96% | 2.92% |

These figures include synchronous ioctl work, but exclude asynchronous driver
work and host passthrough overhead. They are not total driver CPU utilization.
See [profiling methodology and historical results](docs/PROFILING.md) and the
[raw backend data](tests/cpu_profile_backend_results.json).

## Tests and CI

Run hardware-free regressions with Python 3 and an ASan/UBSan-capable C compiler:

```sh
python3 tests/test_logic.py
python3 tests/test_edid.py
python3 tests/test_bridge.py
python3 tests/test_driver.py
python3 tests/test_interrupt.py
python3 tests/test_tools.py
```

For synthetic USB decoding and analysis callbacks, install headless TShark with
Lua support and run as a regular user:

```sh
python3 tests/test_fl2k.py
```

The formatter uses the checked-in Linux-derived `.clang-format` and defaults
to clang-format 18. Checkpatch prefers installed kernel tools, with a pinned
v7.0 download fallback. Both checks fail on findings:

```sh
scripts/clang-format.sh --dry-run
scripts/checkpatch.sh
```

GitHub Actions builds all three backends, runs sanitizer and script tests,
and compiles the guest C utilities without hardware. SonarCloud configuration
lives entirely in `.github/workflows/sonarcloud.yaml` and uses the repository
`SONAR_TOKEN` secret. It captures default and BMI2 builds so both optimized
implementations enter the analysis database. Fork pull requests and projects
without the secret skip the scan.

## Further documentation

- [Driver architecture and backend interface](docs/ARCHITECTURE.md)
- [Development tools, USB capture, and remaining work](docs/DEVELOPMENT.md)
- [Hardware investigation and QEMU workaround](docs/HARDWARE.md)
- [Profiling methodology and results](docs/PROFILING.md)
