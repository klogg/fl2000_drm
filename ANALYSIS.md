# Analysis of captured stream of working Windows driver

Following flow is captured with Wireshark on Windows host with stock Windows driver from FrescoLogic:
1. USB-HDMI dongle with FL2000 and IT66121 connected to PC
2. HDMI display with 800x480 resolution connected
3. Display disconnected
4. Dongle disconnected

Dump of correctly working windows driver with non-standard HDMI display is [available on Dropbox](https://www.dropbox.com/s/niizuk2d4xrkcmg/fl2000.pcapng?dl=0) (45MB pcapng). Use filter below to filter our only FL2000 transactions:<br>
`(usb.addr == "1.4.0" || usb.addr == "1.4.1" || usb.addr == "1.4.2" || usb.addr == "1.4.3")`

NOTE: by default USBPcap captures 65535 bytes max, so packets with frame data are incomplete. For details see https://github.com/desowin/usbpcap/issues/26

## Using Wireshark dissector

To start Wireshark dissector please use following command:<br>
`wireshark -X lua_script:fl2k.lua -r fl2000.pcapng`

NOTE: for correct dissector operation Wireshark must be built with patch allowing dissection of CONTROL messages with "device" recipient. For details see https://code.wireshark.org/review/32626

## Known issues

1. Empty 'status' stage CONTROL packets and zero-size BULK packets are not dissected because of Wireshark implementation - see `dissect_usb_payload()` function not proceeding to dissection when there is no data beyond standard USB header

## Observations

* All video frames are transmitted in RGB24 which is confirmed by USB data frame size:<br>
800x480 (resolution) x3 (bytes per pixel) = 1152000
* EO frame signaled by zero-sized BULK packet
* First BULK frame has timestamp of 40.036560, last BULK packet has timestamp 50.371687, with total 681 ob non-zero BULK frames transmitted - this give us 65.9 FPS which seem to be a strange number.
* Something happens starting sequence 8533
  * set bit 25 in register 0x803C (0xD701084D -> 0xD501084D)
  * receive interrupt (1) with status 0xA8087EE1
