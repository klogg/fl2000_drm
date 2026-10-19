# Ubuntu packages and PPA preparation

The Debian source package `fl2000-drm` builds two binary packages:

- `fl2000-dkms` installs module sources and uses DKMS to build `fl2000.ko`
  and the adapted `it66121.ko` for supported installed kernels.
- `fl2000-enumerator` installs the discovery program, configuration, udev rule
  and systemd service. It depends on the matching DKMS package.

The initial supported target is Ubuntu 26.04 amd64 with Linux 7.0. DKMS skips
other architectures and kernel series. Future kernel support requires explicit
validation and an update to the build restriction. The ordinary source build
retains its existing architecture and backend selection.

## Build locally

Use an Ubuntu 26.04 environment. Install the packaging dependencies:

```sh
sudo apt install build-essential debhelper dh-dkms python3 lintian
```

Build from a clean checkout. The original source tarball contains the tracked
upstream files; Debian packaging and vendored helper sources are carried in the
Debian tarball. The version in `debian/changelog` selects the release and DKMS
source version.

```sh
upstream=$(dpkg-parsechangelog -S Version | sed 's/-[^-]*$//')
git archive --format=tar --prefix="fl2000-drm-$upstream/" HEAD -- . ':!debian' |
    xz > "../fl2000-drm_$upstream.orig.tar.xz"
dpkg-buildpackage -us -uc
lintian ../fl2000-drm_*_amd64.changes
```

A package build checks helper checksums and runs the hardware-free enumerator
tests. It does not build modules against the packaging host's kernel. DKMS
compiles them on the target system. The sources in
`debian/vendor/linux-7.0/` are unmodified Linux v7.0 files with the same checksums
as `scripts/fetch-bridge-connector.sh`; they permit offline DKMS rebuilds.

## Install and remove

Install headers matching the target kernel before installing both packages:

```sh
sudo apt install linux-headers-$(uname -r)
sudo apt install ../fl2000-dkms_*.deb ../fl2000-enumerator_*.deb
dkms status
modinfo -n fl2000
modinfo -n it66121
```

DKMS-installed modules should resolve under `updates/dkms`. Module loading and
bridge discovery follow ordinary USB/I2C autoloading and udev activation.
Connect or reconnect the adapter after installation. No package hook force-loads
the modules or starts an enumeration instance for existing devices.

The adapted `it66121` module shares its name with the stock driver and takes
precedence while installed. Stock module files are not overwritten. A built-in
stock driver cannot be replaced this way. Native OF bridge boards remain outside
this package's validated target; package installation is not native-board
compatibility validation.

Secure Boot uses Ubuntu's DKMS signing and certificate-enrollment process.
Archive signing, including a PPA's signing key, does not enroll the local module
signing key. Test Secure Boot separately on a suitable system.

Disconnect the adapter before upgrading or removing the driver; package removal
does not unload modules in use. Local edits to `/etc/fl2000/enumerator.conf`
survive upgrades and ordinary removal. Purging the enumerator removes its
configuration:

```sh
sudo apt remove fl2000-enumerator fl2000-dkms
sudo apt purge fl2000-enumerator
```

## Package validation

`debian/tests/dkms` rebuilds and installs both modules for the running supported
kernel and checks module lookup and vermagic. It requires an isolated VM with
matching headers; it does not load modules. `debian/tests/enumerator` checks the
installed program, service and udev rule. The autopkgtest control file declares
the dependencies and VM isolation requirement.

Also test fresh installation, a Debian-revision upgrade, preservation of edited
configuration, ordinary removal, purge, a second supported kernel, and rejection
of unsupported kernel series. Run USB passthrough tests in QEMU using the modules
built from the installed DKMS package, and exercise actual udev/systemd service
activation. Building packages on the host does not validate those paths.

## Prepare a PPA upload

The source format is `3.0 (quilt)` and the changelog distribution is `resolute`.
Use a new, increasing Debian revision for every upload. Include the original
source tarball on the first upload of each upstream version. For example, a
revision can advance from `0.0.1-0ubuntu1~ppa1` to
`0.0.1-0ubuntu1~ppa2` without changing the upstream source version.

After local binary-package and DKMS testing, create a source-only upload signed
with an OpenPGP key registered to the uploading Launchpad account:

```sh
dpkg-buildpackage -S -sa -kKEYID
dput ppa:ACCOUNT/PPA ../fl2000-drm_*_source.changes
```

These commands publish to an external service. Preparing local unsigned source
artifacts needs neither a Launchpad account nor a signing key:

```sh
dpkg-buildpackage -S -sa -us -uc
```

Launchpad builds the binary packages; DKMS builds kernel modules on users'
systems. A successful PPA build therefore does not replace DKMS and hardware
validation against the supported Ubuntu kernels.
