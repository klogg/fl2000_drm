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

## Register access statistics

**FL2000**

```
0x0070	10	10
0x0078	4	4
0x8000	8	0
0x8004	4	4
0x8008	2	2
0x800C	2	2
0x8010	2	2
0x8014	2	2
0x801C	2	2
0x8020	1926	836
0x8024	568	0
0x8028	0	264
0x802C	2	2
0x803C	14	14
0x8048	6	6
0x8088	2	2
```
**IT66121**
```
0x00	1	0
0x04	17	9
0x08	2	1
0x0C	22	19
0x10	29	22
0x14	147	29
0x30	4	4
0x34	2	2
0x58	1	2
0x5C	0	1
0x60	14	10
0x64	7	5
0x68	9	5
0x6C	3	2
0x70	4	2
0x90	0	1
0x94	1	1
0x98	3	3
0xC0	5	3
0xC4	4	3
0xCC	2	2
0xE0	2	2
0xE4	1	1
0xF8	3	3
```

## Observations

* All video frames are transmitted in RGB24 which is confirmed by USB data frame size:<br>
800x480 (resolution) x3 (bytes per pixel) = 1152000
* EO frame signaled by zero-sized BULK packet
* First BULK frame has timestamp of 40.036560, last BULK packet has timestamp 50.371687, with total 681 ob non-zero BULK frames transmitted - this give us 65.9 FPS which seem to be a strange number.
* Display connect occur on sequence 1530
  * receive interrupt (1) with status 0x680000E1
    - External monitor event: external monitor connected
* Display disconnect occur on sequence 8830
  * receive interrupt (1) with status 0x480AA260
    - External monitor event: external monitor disconnected
* Something happens starting sequence 8533 - driver forces VGA connect and receives corresponding interrupt from EDID VGA detection circuitry. This does not seem to be correct because we use HDMI monitor, not VGA
  * set bit 25 in register 0x803C (0xD701084D -> 0xD501084D): Force VGA Connect
  * receive interrupt (1) with status 0xA8087EE1
    - EDID event: VGA monitor disconnected
* VGA connect status and VGA DAC power up status correctly follow connection status
* No errors observed during operation except LBUF overflow - but probably this is due too high FPS (66 vs expected 30)
* It is interesting to note that on disconnect amount of frames transmitted is 680 which is 1 less than BULK frames on USB bus - probably disconnect circuitry took some time to raise interrupt so 1 extra frame got transmitted and lost
