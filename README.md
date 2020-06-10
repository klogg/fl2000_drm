[![Build Status](https://travis-ci.org/klogg/fl2000_drm.svg?branch=master)](https://travis-ci.org/klogg/fl2000_drm) [![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=klogg_fl2000_drm&metric=alert_status)](https://sonarcloud.io/dashboard?id=klogg_fl2000_drm)

<h1>
  
```diff
-= NOTICE: THIS DRIVER IS NOT YET WORKING PROPERLY =-
```

</h1>

# Linux kernel FL2000DX/IT66121FN dongle DRM driver

Clean re-implementation of FrescoLogic FL2000DX DRM driver and ITE Tech IT66121F driver, allowing to enable full display controller capabilities for [USB-to-HDMI dongles](https://www.aliexpress.com/item/HD-1080P-USB-3-0-To-HDMI-External-Video-Graphic-Card-Multi-Display-Cable-Adapter-Converter/32808836824.html?spm=a2g0s.9042311.0.0.4a9f4c4dow19O6) based on such chips in Linux

## FL2000DX
The FL2000DX is Fresco Logic’s USB 3.0 Display device controller. It integrates Fresco Logic’s display transfer engine, USB 3.0 device controller, USB 3.0 transceiver, and a VGA (D-Sub) DAC. Fresco Logic’s display transfer engine is designed with Fresco Logic’s proprietary architecture and processes the video stream optimally for USB 3.0 bandwidth. The high performance video DAC provides clear and crisp display quality, and supports full HD (1920×1080) resolution. The integrated USB 3.0 device controller and transceiver were developed in conjunction with the de-facto standard Fresco USB 3.0 host controllers, which ensures the best performance and compatibility.

## IT66121FN
The IT66121 is a high-performance and low-power single channel HDMI transmitter, fully compliant with HDMI 1.3a, HDCP 1.2 and backward compatible to DVI 1.0 specifications. IT66121 also provide the HDMI1.4 3D feature, which enables direct 3D displays through an HDMI link. The IT66121 serves to provide the most cost-effective HDMI solution for DTV-ready consumer electronics such as set-top boxes, DVD players and A/V receivers, as well as DTV-enriched PC products such as notebooks and desktops, without compromising the performance. Its backward compatibility to DVI standard allows connectivity to myriad video displays such as LCD and CRT monitors, in addition to the ever-so-flourishing Flat Panel TVs.

## Implementation

### Driver structure
![Diagram](fl2000.svg)

All registers (both FL2000 and IT66121) access is implemented via regmaps. It is assumed that FL2000DX outputs DPI interface (kind of "crtc" output, not "encoder") that is connected to HDMI or other transciever. USB Bulk Streams are not supported by FL2000DX, so implementation will simly use Bulk endpoint.

See [debug section](https://github.com/klogg/fl2000_drm/blob/master/DEBUG.md) and [analysis section](https://github.com/klogg/fl2000_drm/blob/master/ANALYSIS.md) for more details on development.

### Endpoints addresses HW bug
Recent kernels have a fix that checks duplication of USB endpoint numbers across interfaces which is an issue due to HW design bug: it uses same endpoint #1 across interfaces 1 and 2, which is not allowed by USB specification:endpoint addresses can be shared only between alternate settings, not between interfaces. Kernel log with issue looks like this:
```
new SuperSpeed Gen 1 USB device number 2 using xhci_hcd
config 1 interface 1 altsetting 0 has a duplicate endpoint with address 0x81, skipping
config 1 interface 1 altsetting 0 has a duplicate endpoint with address 0x1, skipping
config 1 interface 1 altsetting 1 has a duplicate endpoint with address 0x81, skipping
config 1 interface 1 altsetting 1 has a duplicate endpoint with address 0x1, skipping
config 1 interface 1 altsetting 2 has a duplicate endpoint with address 0x81, skipping
config 1 interface 1 altsetting 2 has a duplicate endpoint with address 0x1, skipping
config 1 interface 1 altsetting 3 has a duplicate endpoint with address 0x81, skipping
config 1 interface 1 altsetting 3 has a duplicate endpoint with address 0x1, skipping
config 1 interface 1 altsetting 4 has a duplicate endpoint with address 0x81, skipping
config 1 interface 1 altsetting 4 has a duplicate endpoint with address 0x1, skipping
config 1 interface 1 altsetting 5 has a duplicate endpoint with address 0x81, skipping
config 1 interface 1 altsetting 5 has a duplicate endpoint with address 0x1, skipping
config 1 interface 1 altsetting 6 has a duplicate endpoint with address 0x81, skipping
config 1 interface 1 altsetting 6 has a duplicate endpoint with address 0x1, skipping
```
In order to workaround this same as original driver we use default altsetting (#0) of streaming interface (#1) with bulk transfers.

## How to use
**IMPORTANT!** As it is seen from the original driver sources FL2000 does not properly support USB3 U1/U2 LPM. While the dongle was working properly woth desktop Linux machine, on the laptop with Linux the dongle had issues because USB hub was setting U1/U2 timers despite LPM configuration was disabled in the driver. Issues observed were: all interrupt URBs were not delivered, sometimes control URBs were not delivered. This can *probably* be fixed using [Linux USB device quirks](https://elixir.bootlin.com/linux/latest/source/drivers/usb/core/quirks.c), e.g. with kernel boot param:<br> `quirks=1D5C:2000:USB_QUIRK_NO_LPM`

## Limitations (not implemented)
 * D-sub
 * HDMI CEC
 * HDMI Audio
 * HDCP
 * USB 2.0
 * Dongle onboard SPI EEPROM access via USB Mass Storage

## Known issues
 * Connecting more than one dongle to the same USB bus may not work
 * Non big-endian hosts (e.g. little endian) may not work
 * 32-bit hosts may not work

## Upstreaming
Considering, no firm decision yet. Current design uses unsafe components linking and non-standard I2C device class for autodetection, this has to be addresed prior to upstreaming

## Sources
 * Original driver by FrescoLogic: https://github.com/FrescoLogic/FL2000
 * Major clean-up of original FL driver by Hans Ulli Kroll: https://github.com/ulli-kroll/FL2000
 * Reference IT66121FN driver ftom RK3188 Android KK kernel repositpry: https://github.com/phjanderson/Kernel-3188
 * Reference USB DRM implementation of DisplayLink driver: https://elixir.bootlin.com/linux/latest/source/drivers/gpu/drm/udl
 * Reference simple DRM implementation of PL111 driver: https://elixir.bootlin.com/linux/latest/source/drivers/gpu/drm/pl111

## Documentation
Not shared here due to NDAs, Copyrights, etc.
 * FL2000 MP Memory Mapped Address Space Registers
 * IT66121FN Datasheet v1.02
 * IT66121 Programming Guide v1.05
 * IT66121 Register List Release V1.0
 * AV BDP v1.0
 * CEA-861-D
 * VESA-EEDID-A2

## Notes
 * VGA (D-Sub) DAC output of FL2000DX can be implemented as a DRM bridge (dumb_vga_dac)
 * For registration of sibling I2C devices of IT66121 (CEC, ...) i2c\_new\_dummy() function may be used

## TODO
 * Review, test and cleanup init/cleanup procedures to ensure no leaks or races or other issues
 * Debug and fix deinitialization (shutdown / remove)
 * Move all (or as much as possible) resources to device resources (dev/devres functions) and simplify release
 * Replace bus detection & components linking with configuration (modprobe / udev)
 * Allow driver to be builtin to kernel
 * Refactor for better / cleaner structure and modularity
 * Implement unit testing with latest kernel & DRM unit testing tools, target coverage shall be 100%
 * Switch to isochronous transfers
 * Add computation for PLL settings from selected monitor mode
 * Implement suspend / resume
