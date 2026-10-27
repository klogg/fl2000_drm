# FL2000 USB display driver

[![Build](https://github.com/klogg/fl2000_drm/actions/workflows/makefile.yml/badge.svg)](https://github.com/klogg/fl2000_drm/actions/workflows/makefile.yml)
[![Stylecheck](https://github.com/klogg/fl2000_drm/actions/workflows/codingstyle.yaml/badge.svg)](https://github.com/klogg/fl2000_drm/actions/workflows/codingstyle.yaml)
[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=klogg_fl2000_drm&metric=alert_status)](https://sonarcloud.io/dashboard?id=klogg_fl2000_drm)

An out-of-tree DRM/KMS driver for Fresco Logic FL2000 USB display adapters
(USB ID `1d5c:2000`), targeting **Linux 7.0**. The core connects to an external
bridge; the supplied IT6610/21/22 module adapts Linux v7.0's bridge driver for
firmware-less attachment and the controller's I2C/reset constraints. Native VGA
is not implemented.

Hardware testing covers one IT66121 HDMI adapter through QEMU/KVM. Other kernel
versions require API review. See [validation scope and backlog](docs/DEVELOPMENT.md#validation-scope)
for tested configurations and remaining limits.

## Build

Install a C compiler, Make, and headers matching the target Linux 7.0 kernel.
The kernel header build tools also need their runtime libraries (including
libdw for gendwarfksyms on the tested Ubuntu headers).
Build against the running kernel, or select an installed header tree.
For distribution kernels lacking `CONFIG_DRM_BRIDGE_CONNECTOR` or its HDMI
helpers, first run `scripts/fetch-bridge-connector.sh`. It prepares checksum-pinned,
unmodified Linux 7.0 sources in the ignored `.qemu-build/upstream/` directory,
using vendored files when present or HTTPS with curl otherwise. External builds
compile only missing helpers.
Alternatively, set `FL2000_HELPER_SOURCE_DIR` to the matching kernel source
`drivers/gpu/drm/display/` directory. In-tree builds use Kconfig-selected helpers.

```sh
scripts/fetch-bridge-connector.sh
```

Build the modules:

```sh
make
make KSRC=/usr/src/linux-headers-7.0.0-38-generic
```

The resulting modules are `fl2000.ko` and `it66121.ko`. Use this repository's
adapted bridge module; the stock upstream module is not interchangeable.
The FL2000 core creates the I2C adapter; the userspace service identifies
and instantiates its bridge as described below. There is no bridge-selection
module parameter.

`FL2000_FRAME_BACKEND=auto` selects SSSE3 for x86-64 target kernels and portable
code elsewhere. Explicit choices are `ssse3`, `bmi2`, and `generic`. Optimized
backends fall back to portable processing when CPU features are unavailable.

```sh
make FL2000_FRAME_BACKEND=generic
```

If module signing is needed, `scripts/sign.sh` signs both modules. Run
`scripts/sign.sh --help` for signing-key and tool overrides. Signing does not
by itself enroll the certificate in the target kernel's trust store.

### Ubuntu packages

PPA-ready Debian packaging builds separate DKMS and bridge-enumerator packages
for Ubuntu 26.04 amd64 with Linux 7.0. See [packaging](docs/PACKAGING.md) for local
builds, installation, module precedence and source-upload preparation.

### In-tree integration

Copy the driver sources, `bridge/`, `frame/`, `Kbuild`, and `Kconfig` into
`drivers/gpu/drm/fl2000/`. Add `source "drivers/gpu/drm/fl2000/Kconfig"` to
the DRM Kconfig and `obj-$(CONFIG_DRM_FL2000) += fl2000/` to its Makefile.
Merge this repository's `MAINTAINERS` entry into the kernel's file.

Enable `CONFIG_DRM_FL2000` and choose its frame preparation backend. Kconfig
selects the DRM bridge-connector and shmem helpers and limits accelerated
backends to supported architectures. Select `CONFIG_DRM_FL2000_IT66121`
separately when using the supplied bridge.
It shares the standard module name, so `CONFIG_DRM_ITE_IT66121` must be disabled
only when that optional adaptation is selected.
The external build continues to accept `FL2000_FRAME_BACKEND` overrides.

## Bridge configuration on x86

FL2000 registers an I2C adapter without assuming which bridge is attached.
Device Tree and a kernel rebuild are not required for userspace enumeration.
The core supplies adapter software-node properties for transport constraints,
pixel input wiring and reset ownership. It also exposes the bridge IRQ through
an IRQ domain keyed by that software node. The bridge reads these settings
without identifying the controller. These local interfaces are described in
[the architecture](docs/ARCHITECTURE.md);
standard I2C quirks separately describe transfer limits. On x86 the bridge
uses component attachment and this IRQ domain, without an OF node. Existing
Device Tree boards retain firmware-graph attachment.
Install the four enumeration components after installing the driver modules:

```sh
sudo make install-enumerator
sudo udevadm control --reload-rules
```

The runtime requires POSIX shell, awk, base utilities, `flock` (util-linux),
`modprobe` (kmod), and `i2ctransfer` (i2c-tools). No Python is used.
On connection, udev activates one systemd service per FL2000 I2C adapter. The
service reads `/etc/fl2000/enumerator.conf`, identifies a configured bridge
using register reads, and creates its client through I2C `new_device`.
The shipped configuration covers IT66121 at 0x4c and 0x4d. Bridge identities,
addresses, module names, and the binding timeout reside in that configuration;
the shell program contains no bridge-specific identification data.

See [enumeration configuration](docs/ENUMERATION.md) for the format, manual
activation on an already-connected adapter, and diagnostics. Installation
preserves an existing configuration file. The service stops if its adapter
is removed; the I2C core removes its clients automatically.

A bridge driver still needs the component binding and standard IRQ handling
described in [the architecture](docs/ARCHITECTURE.md). Enumeration alone does
not adapt an arbitrary upstream bridge driver. The QEMU startup fixture uses
the same enumerator and configuration in its minimal guest. Supply
`--i2ctransfer PATH` if i2c-tools is not installed on the host.

## Isolated QEMU testing

These procedures load modules inside QEMU guests. Requirements are QEMU with KVM
and USB-host support, access to `/dev/kvm` and the selected USB port, and the
matching kernel image in `/boot`. Containers must expose those device nodes.
The hardware and profiling launchers also need host `flock` and
`i2ctransfer`, or an explicit `--i2ctransfer PATH`. They include these tools
and their shared libraries in the guest overlay. The minimal guest needs
POSIX shell, awk, base utilities, and built-in or preloaded `i2c-dev` support.
The tested QEMU 11.1.2 needs the included
[duplicate-endpoint workaround](docs/HARDWARE.md#qemu-1112-workaround).
High-resolution tests can also use the optional
[bulk-OUT pipeline patch](docs/HARDWARE.md#bulk-transfer-queueing).

Supply your own `base-initramfs.cpio.gz`; it is **not included**. Its init must
provide a root shell, mounted `/proc`, `/sys`, and `/dev`, and module tools.
The launchers collect installed dependency modules for the selected kernel and
include a guest loader in the overlay. Matching dependency modules must exist
on the host. Compressed `.zst` modules need Python 3.14 or the zstd command.
The hardware-free smoke test also loads these dependencies from its replacement
`/init`. The standalone computation benchmark replaces `/init` and includes only
its benchmark module. Base utilities, mounted filesystems and USB host support
must be available when each guest procedure needs them; included module files
alone do not arrange guest boot or USB-controller setup.

The bridge callback module stays referenced until the DRM device and its open
files are released. Unload `fl2000` first and close outstanding DRM files before
unloading `it66121`.

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

Startup loads dependency modules, preloads bridge candidates that are not
already registered or loaded, and loads FL2000 before enumeration. It is safe
to repeat. No host USB controller is unbound. Logs default to
`.qemu-build/fl2000-tests`. Use `KVER`, `MODULE_DIR`, or `QEMU_OUTPUT_DIR` to
override the wrapper's defaults.

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

The FL2000 display path requires an external bridge component. The shipped
enumeration data identifies IT66121. The bridge module also retains normal
firmware-described IT6610/21/22 operation. Other bridges on FL2000 need
compatible component attachment, IRQ handling, and hardware configuration;
adding their identities to the config alone does not provide driver support.
Native VGA through FL2000's integrated DAC is not implemented. A missing
bridge is not treated as a VGA adapter.

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
[validation backlog](docs/DEVELOPMENT.md#validation-backlog) and
[remaining implementation work](docs/DEVELOPMENT.md#remaining-implementation-work).

## Performance

Frame preparation is the main measured guest CPU cost. The default x86-64
SSSE3 backend accelerates XRGB8888 conversion; native RGB888/RGB565 use portable
wire reordering. Damage clips reduce preparation work, while USB still sends
complete frames. Measurements must include asynchronous and host work before
being described as total driver CPU.

See [profiling](docs/PROFILING.md) for the Portable / SSSE3 / BMI2 comparison,
measurement procedures and raw data. CPU tables measure revision `f9e99ef`;
transport tables measure `95a2a7c`.

## Tests and CI

Run hardware-free regressions with Python 3 and an ASan/UBSan-capable C compiler:

```sh
for test in logic register_locking registers edid bridge bridge_probe \
    driver drm interrupt tools enumerator; do
    UBSAN_OPTIONS=halt_on_error=1 python3 "tests/test_$test.py" || exit 1
done
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

- [Bridge enumeration and service configuration](docs/ENUMERATION.md)
- [Driver architecture and backend interface](docs/ARCHITECTURE.md)
- [Development, USB capture and validation backlog](docs/DEVELOPMENT.md)
- [Hardware constraints and QEMU workaround](docs/HARDWARE.md)
- [Profiling methodology and results](docs/PROFILING.md)
