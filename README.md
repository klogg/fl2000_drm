[![Gitter](https://badges.gitter.im/fl2000_drm/community.svg)](https://gitter.im/fl2000_drm/community?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge) [![Build Status](https://travis-ci.org/klogg/fl2000_drm.svg?branch=master)](https://travis-ci.org/klogg/fl2000_drm) [![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=klogg_fl2000_drm&metric=alert_status)](https://sonarcloud.io/dashboard?id=klogg_fl2000_drm)

# Linux kernel FL2000DX/IT66121FN dongle DRM driver

Clean re-implementation of FrescoLogic FL2000DX DRM driver and ITE Tech IT66121F driver, allowing to enable full display controller capabilities for [USB-to-HDMI dongles](https://www.aliexpress.com/item/32821739801.html?spm=a2g0o.productlist.0.0.14ee52fb8rFfu5) based on such chips in Linux

### Building driver

Check out the code and type
```
make
```
Use
```
insmod fl2000.ko && insmod it66121.ko
```
with sudo or in root shell to start the driver. If you are running on a system with secure boot enabled, you may need to sign kernel modules. Try using provided script for this:
```
./scritps/sign.sh
```

**NOTE:** proper kernel headers and build tools (e.g. "build-essential" package) must be installed on the system. Driver is developed and tested on Ubuntu 20.04 with Linux kernel 5.4, so better to test it this way.

For more information check project's [Wiki](https://github.com/klogg/fl2000_drm/wiki)
