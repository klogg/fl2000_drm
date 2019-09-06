# Linux kernel FL2000DX/IT66121FN dongle DRM driver

Clean re-implementation of FrescoLogic FL2000DX DRM driver and ITE Tech IT66121F driver, allowing to enable full display controller capabilities for [USB-to-HDMI dongles](https://www.aliexpress.com/item/HD-1080P-USB-3-0-To-HDMI-External-Video-Graphic-Card-Multi-Display-Cable-Adapter-Converter/32808836824.html?spm=a2g0s.9042311.0.0.4a9f4c4dow19O6) based on such chips in Linux

## FL2000DX
The FL2000DX is Fresco Logic’s USB 3.0 Display device controller. It integrates Fresco Logic’s display transfer engine, USB 3.0 device controller, USB 3.0 transceiver, and a VGA (D-Sub) DAC. Fresco Logic’s display transfer engine is designed with Fresco Logic’s proprietary architecture and processes the video stream optimally for USB 3.0 bandwidth. The high performance video DAC provides clear and crisp display quality, and supports full HD (1920×1080) resolution. The integrated USB 3.0 device controller and transceiver were developed in conjunction with the de-facto standard Fresco USB 3.0 host controllers, which ensures the best performance and compatibility.

## IT66121FN
The IT66121 is a high-performance and low-power single channel HDMI transmitter, fully compliant with HDMI 1.3a, HDCP 1.2 and backward compatible to DVI 1.0 specifications. IT66121 also provide the HDMI1.4 3D feature, which enables direct 3D displays through an HDMI link. The IT66121 serves to provide the most cost-effective HDMI solution for DTV-ready consumer electronics such as set-top boxes, DVD players and A/V receivers, as well as DTV-enriched PC products such as notebooks and desktops, without compromising the performance. Its backward compatibility to DVI standard allows connectivity to myriad video displays such as LCD and CRT monitors, in addition to the ever-so-flourishing Flat Panel TVs.

## Implementation
![Diagram](fl2000.svg)

All registers (both FL2000 and IT66121) access is implemented via regmaps. It is assumed that FL2000DX outputs DPI interface (kind of "crtc" output, not "encoder") that is connected to HDMI or other transciever. USB Bulk Streams are not supported by FL2000DX, so implementation will simly use Bulk endpoint.

See [debug section](https://github.com/klogg/fl2000_drm/blob/master/DEBUG.md) for more details on development.

## How to use
**IMPORTANT!** As it is seen from the original driver sources FL2000 does not properly support USB3 U1/U2 LPM. While the dongle was working properly woth desktop Linux machine, on the laptop with Linux the dongle had issues because USB hub was setting U1/U2 timers despite LPM configuration was disabled in the driver. Issues observed were: all interrupt URBs were not delivered, sometimes control URBs were not delivered. This can *probably* be fixed using [Linux USB device quirks](elixir.bootlin.com/linux/latest/source/drivers/usb/core/quirks.c), e.g. with kernel boot param:<br> `quirks=1D5C:2000:USB_QUIRK_NO_LPM`

## Limitations
 * HDMI CEC is not supported
 * HDMI Audio is not supported
 * HDCP is not supported
 * USB2.0 is not supported
 * Dongle onboard SPI EEPROM access via USB Mass Storage not implemented

## Known issues
 * Connecting more than one dongle to the same USB bus may not work
 * Non big-endian hosts (e.g. little endian) may not work
 * 32-bit hosts may not work

## Upstreaming
Considering, no firm decision yet

## Sources
 * Original driver by FrescoLogic: https://github.com/FrescoLogic/FL2000
 * Major clean-up of original FL driver by Hans Ulli Kroll: https://github.com/ulli-kroll/FL2000
 * Reference IT66121FN driver ftom RK3188 Android KK kernel repositpry: https://github.com/phjanderson/Kernel-3188
 * Reference USB DRM implementation of DisplayLink driver (see drivers/gpu/drm/udl in kernel sources)
 * Reference simple DRM implementation of PL111 driver (see drivers/gpu/drm/pl111 in kernel sources)

## Notes
 * VGA (D-Sub) DAC output of FL2000DX can be implemented as a DRM bridge (dumb_vga_dac)
 * For registration of sibling I2C devices of IT66121 (CEC, DDC, ...) i2c\_new\_dummy() function may be used
 * Need to review, test and cleanup init/cleanup procedures to ensure no leaks or races or other issues
 * Need to implement unit testing with latest kernel & DRM unit testing tools, target coverage shall be 100%
 * Need to implement static code analysis with Coverity Scan and maybe some other tools

## Open questions
 * How to simultaneously support HDMI bridge and D-Sub bridge? Config option?
 * I2C autodetection require non-empty class. Is it safe to introduce custom one? Need to ask community
