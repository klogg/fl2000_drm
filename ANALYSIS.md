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

NOTE: for correct dissector operation Wireshark must be built with patch allowing dissection of CONTROL messages with "device" recipient. For details see https://code.wireshark.org/review/32626 (merged to Wireshark master development branch)

### Known issues

* Empty 'status' stage CONTROL packets and zero-size BULK packets are not dissected because of Wireshark implementation - see `dissect_usb_payload()` function not proceeding to dissection when there is no data beyond standard USB header

## Observations

See parsed stream dump and register access statistics below.

* All video frames are transmitted in RGB24 which is confirmed by USB data frame size:<br>
800x480 (resolution) x3 (bytes per pixel) = 1152000
* EO frame signaled by zero-sized BULK packet
* First BULK frame has timestamp of 40.036560, last BULK packet has timestamp 50.371687, with total 681 ob non-zero BULK frames transmitted - this give us 65.9 FPS which seem to be correct
* Display connect occur on sequence 1530
  * receive interrupt (1) with status 0x680000E1
    - External monitor event: external monitor connected
* Display disconnect occur on sequence 8830
  * receive interrupt (1) with status 0x480AA260
    - External monitor event: external monitor disconnected
* Not clear what "Force VGA Connect" bit is responsible for. In Linux implementation this bit is set on reconfiguring IT66121 (infoframes, AFE, etc.) and immediately reset after reconfiguration. In Windows it is reset after 537 operational frames were sent (see sequence 8533). When this bit reset, FL2000 shoots interrupt from EDID VGA detection circuitry.
  * reset bit 25 in register 0x803C (0xD701084D -> 0xD501084D): Force VGA Connect
  * receive interrupt (1) with status 0xA8087EE1
    - EDID event: VGA monitor disconnected
* VGA connect status and VGA DAC power up status correctly follow connection status
* No errors observed during operation except LBUF overflow
* It is interesting to note that on disconnect amount of frames transmitted is 680 which is 1 less than BULK frames on USB bus - probably disconnect circuitry took some time to raise interrupt so 1 extra frame got transmitted and lost
* There is (seemingly) concurrent access to registers happened leading to some sort of mixup when forcing VGA connect and starting IT66121 I2C register access. Linux implementation is different and does not have this issue.
* There are 2 extra application resets besides the one on the start of the driver
* In EDID section, it is seen how driver workarounds the problem of missing 3 first bytes of every EDID read operation: Ask for 6 bytes that will overlap the missing 3 bytes for every 32-bytes read. This leads to 7 read blocks while this could be brought done by 5 only significantly amount of activities:
  * Fill 3 bytes with '00 FF FF'
  * Read 29 bytes from offset 3
  * Read 29 bytes from offset 32
  * Read 29 bytes from offset 61
  * Read 29 bytes from offset 90
  * Read 9 bytes from offset 119
* When reading EDID original driver seem to reset 2 high bytes (readonly as per doc) to 0, in capture we see that windows driver just keeps them 'dirty' with previous operation result
* DDC Abort issued twice during EDID, with EDID_ROM flag enabled (flag setting is absent in original driver)

## The "BIG TABLE"

Original FL2000 Linux driver implements array of hardcoded register values to properly configure timings and pixel clock for different video modes. All this can be dynamically calculated, and most probably is done in Windows driver that provide configuration for 800x480 display which is unavailable in Linux. Full dump of big_table and analysis is [available on Dropbox](https://www.dropbox.com/s/2xb9j8pz4yjyrsb/big_table_analysis.ods?dl=0)

Pixel clock is acheived with PLL connected to main XTAL (10MHz in our case)
* Pixel clock = XTAL clock / (VGA_PLL_REG.pll_pre_div & 0xF) * VGA_PLL_REG.pll_multi / VGA_PLL_REG.pll_post_div

Lower bits of VGA_PLL_REG.pll_pre_div has possible values of 1 or 2. Not sure if other values are allowed

It is unclear though what is the purpose of the high 4 bits of VGA_PLL_REG.pll_pre_div. In the table values for these 4 bits are 0, 1, 2 or 3 and this is somehow aligned with "intermediate" clock order (after pre divisor and multiplier). With minimum-maximum clock distribution we can gate to some empirical rules setting values 0-3:

| x | min | max  | rule |
|---|-----|------|------|
| 0 | 80  | 100  | -    |
| 1 | 130 | 150  | >100 |
| 2 | 270 | 490  | >250 |
| 3 | 515 | 1000 | >500 |

Not sure if other values are possible. Also it seems that maximum "intermediate" clock in the table is 1GHz

Timings are set according to mode structure:
* H_SYNC_REG_1.hactive = hdisplay
* V_SYNC_REG_1.vactive = vdisplay
* H_SYNC_REG_1.htotal = htotal
* H_SYNC_REG_1.vtotal = vtotal
* H_SYNC_REG_2.hsync_width = hsync_end - hsync_start
* V_SYNC_REG_2.vsync_width = vsync_end - vsync_start
* H_SYNC_REG_2.hstart = (htotal - hsync_start + 1)
* V_SYNC_REG_2.vstart = (vtotal - vsync_start + 1)
* V_SYNC_REG_2.frame start latency = vstart

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
0x0C	17	14
0x10	29	22
0x14	147	29
0x58	1	1
0x60	14	9
0x64	7	4
0x68	3	2
0x70	4	2
0xC0	5	3
0xC4	4	3
0xCC	2	2
0xE0	2	2
0xE4	1	1
0xF8	3	3
0x10C	5	5
0x130	4	4
0x134	2	2
0x158	0	1
0x15C	0	1
0x160	0	1
0x164	0	1
0x168	6	3
0x16C	3	2
0x190	0	1
0x194	1	1
0x198	3	3
```

## Parsed stream

NOTE: when reading the stream, one need to remember that byte access is not possible for I2C connected devices, always dword (4 bytes) is read; thus addresses are always 4-byte aligned.

**FL2000 Reset**

RST_CTRL_REG: Software reset to application logic
```
REG RD 0x8048 : 0x00000004
REG WR 0x8048 : 0x00008004
```
**IT66121 Check connected chip ID and revision**

0x00: Read vendor ID (0x4954) device ID (0x612) revision ID (1)
```
I2C RD IT66121: 0x00 : 0x16124954
```
**IT66121 Initial Reset as per 'Programming Guide'**

0x04: SW Reset
```
I2C RD IT66121: 0x04 : 0x0000601C
I2C RD IT66121: 0x04 : 0x0000601C
I2C WR IT66121: 0x04 : 0x0000603C
```
**IT66121 Initial Power On Sequence as per 'Programming Guide' (fl2000_hdmi_power_up)**

0x0F: power up GRCLK (reset bit 6 in guide), power down IACLK, TxCLK, CRCLK (???)
```
I2C RD IT66121: 0x0C : 0x080C0000
I2C RD IT66121: 0x0C : 0x080C0000
I2C WR IT66121: 0x0C : 0x380C0000
```
0x05: (reset bit 0)
```
I2C RD IT66121: 0x04 : 0x0000601C
I2C RD IT66121: 0x04 : 0x0000601C
I2C WR IT66121: 0x04 : 0x0000601C
```
0x61: power on the DRV (reset bit 5)
```
I2C RD IT66121: 0x60 : 0x188810FF
I2C RD IT66121: 0x60 : 0x188810FF
I2C WR IT66121: 0x60 : 0x188810FF
```
0x62: power on XPLL (reset bits 6 and 2)
```
I2C RD IT66121: 0x60 : 0x188810FF
I2C RD IT66121: 0x60 : 0x188810FF
I2C WR IT66121: 0x60 : 0x188810FF
```
0x64: power on IPLL (reset bit 6)
```
I2C RD IT66121: 0x64 : 0x00000094
I2C RD IT66121: 0x64 : 0x00000094
I2C WR IT66121: 0x64 : 0x00000094
```
0x61: Disable reset for HDMI_TX_DRV (reset bit 4)
```
I2C RD IT66121: 0x60 : 0x188810FF
I2C RD IT66121: 0x60 : 0x188810FF
I2C WR IT66121: 0x60 : 0x188800FF
```
0x62: XP_RESETB (set bit 3)
```
I2C RD IT66121: 0x60 : 0x188800FF
I2C RD IT66121: 0x60 : 0x188800FF
I2C WR IT66121: 0x60 : 0x188800FF
```
0x64: IP_RESETB (set bit 2)
```
I2C RD IT66121: 0x64 : 0x00000094
I2C RD IT66121: 0x64 : 0x00000094
I2C WR IT66121: 0x64 : 0x00000094
```
**(extra steps in fl2000_hdmi_power_up)**

0x6A: AFE magic 00 -> 70
```
I2C RD IT66121: 0x68 : 0xFF003000
I2C WR IT66121: 0x68 : 0xFF703000
```
0x66: AFE magic 00 -> 1F
```
I2C RD IT66121: 0x64 : 0x00000094
I2C WR IT66121: 0x64 : 0x001F0094
```
0x63: 7: AFE maximum output current setting
```
I2C RD IT66121: 0x60 : 0x188800FF
I2C WR IT66121: 0x60 : 0x388800FF
```
0x0F: power up IACLK, TxCLK (???)
```
I2C RD IT66121: 0x0C : 0x380C0000
I2C RD IT66121: 0x0C : 0x380C0000
I2C WR IT66121: 0x0C : 0x080C0000
```
**IT66121 Reset (unnecessary?)**

SW Reset
```
I2C RD IT66121: 0x04 : 0x0000601C
I2C RD IT66121: 0x04 : 0x0000601C
I2C WR IT66121: 0x04 : 0x0000603C
```
**FL2000 Reset (unnecessary?)**

RST_CTRL_REG: Software reset to application logic
```
REG RD 0x8048 : 0x00000004
REG WR 0x8048 : 0x00008004
```
**FL2000 Enable VGA monitor detection (unnecessary?)**

VGA_I2C_SC_REG: VGA EDID detect enable
```
REG RD 0x8020 : 0x8000044C
REG WR 0x8020 : 0xC000044C
```
VGA_I2C_SC_REG: VGA External monitor detect enable
```
REG RD 0x8020 : 0xC000044C
REG WR 0x8020 : 0xD000044C
```
**FL2000 Some HW issue workaround**

VGA Control Register3: Disable an auto-generation of reset when we wakeup from disconnect
```
REG RD 0x8088 : 0x00000488
REG WR 0x8088 : 0x00000088
```
**FL2000 check interrupt status register and enable interrupts**

Interrupt status register empty
```
REG RD 0x8000 : 0x08000000
```
**Monitor connected**

```
INTERRUPT	1
REG RD 0x8000 : 0x680000E1
```
**'Enable' sequence for USB interface**

```
REG RD 0x0070 : 0x04006085
REG WR 0x0070 : 0x04106085
```
```
REG RD 0x0070 : 0x04106085
REG WR 0x0070 : 0x04186085
```
```
REG RD 0x0078 : 0x18010D14
REG WR 0x0078 : 0x18010D14
```
**Read monitor EDID data (fl2000_hdmi_read_edid_table)**

Pattern here is: `rrw10-rw14-rrw10-rw14-rw14-w10-w14`<br>
0x04-0x07: read default values
```
I2C RD IT66121: 0x04 : 0x0000601C
```
0x10: Switch DDC port to PC
```
I2C RD IT66121: 0x10 : 0x633FD796
I2C WR IT66121: 0x10 : 0x633FD701
```
0x15: Clear DDC FIFO
```
I2C RD IT66121: 0x14 : 0xBE82F7F3
I2C WR IT66121: 0x14 : 0xBE8209F3
```
0x0F: Power UP CRCLK
```
I2C RD IT66121: 0x0C : 0x086C0000
I2C WR IT66121: 0x0C : 0x006C0000
```
Block 1
```
I2C RD IT66121: 0x10 : 0x633FD791
I2C RD IT66121: 0x10 : 0x633FD791
I2C WR IT66121: 0x10 : 0x633FD793
```
```
I2C RD IT66121: 0x14 : 0xBE82F9F3
I2C WR IT66121: 0x14 : 0xBE820FF3
```
```
I2C RD IT66121: 0x10 : 0x633FD793
I2C RD IT66121: 0x10 : 0x633FD793
I2C WR IT66121: 0x10 : 0x633FD793
```
```
I2C RD IT66121: 0x14 : 0xD982FFF3
I2C WR IT66121: 0x14 : 0xD9820FF3
```
```
I2C RD IT66121: 0x14 : 0xFF82FFF3
I2C WR IT66121: 0x14 : 0xFF8209F3
```
```
I2C WR IT66121: 0x10 : 0x2000A001
```
```
I2C WR IT66121: 0x14 : 0x20000300
```
```
I2C RD IT66121: 0x14 : 0xFF80F300
I2C RD IT66121: 0x14 : 0xFF80F300
I2C RD IT66121: 0x14 : 0xFF80F300
I2C RD IT66121: 0x14 : 0xFF80F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0480F300
I2C RD IT66121: 0x14 : 0x8180F300
I2C RD IT66121: 0x14 : 0x0480F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x1180F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0380F300
I2C RD IT66121: 0x14 : 0x8080F300
I2C RD IT66121: 0x14 : 0x0F80F300
I2C RD IT66121: 0x14 : 0x0A80F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0A80F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
```
Block 2
```
I2C RD IT66121: 0x10 : 0x2000A091
I2C RD IT66121: 0x10 : 0x2000A091
I2C WR IT66121: 0x10 : 0x2000A093
```
```
I2C RD IT66121: 0x14 : 0x0080F300
I2C WR IT66121: 0x14 : 0x00800F00
```
```
I2C RD IT66121: 0x10 : 0x2000A093
I2C RD IT66121: 0x10 : 0x2000A093
I2C WR IT66121: 0x10 : 0x2000A093
```
```
I2C RD IT66121: 0x14 : 0x0082FF00
I2C WR IT66121: 0x14 : 0x00820F00
```
```
I2C RD IT66121: 0x14 : 0x0082FF00
I2C WR IT66121: 0x14 : 0x00820900
```
```
I2C WR IT66121: 0x10 : 0x061DA001
```
```
I2C WR IT66121: 0x14 : 0x061D0300
```
```
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
```
Block 3
```
I2C RD IT66121: 0x10 : 0x061DA091
I2C RD IT66121: 0x10 : 0x061DA091
I2C WR IT66121: 0x10 : 0x061DA093
```
```
I2C RD IT66121: 0x14 : 0xFF80F300
I2C WR IT66121: 0x14 : 0xFF800F00
```
```
I2C RD IT66121: 0x10 : 0x061DA093
I2C RD IT66121: 0x10 : 0x061DA093
I2C WR IT66121: 0x10 : 0x061DA093
```
```
I2C RD IT66121: 0x14 : 0x0482FF00
I2C WR IT66121: 0x14 : 0x04820F00
```
```
I2C RD IT66121: 0x14 : 0x0082FF00
I2C WR IT66121: 0x14 : 0x00820900
```
```
I2C WR IT66121: 0x10 : 0x2020A001
```
```
I2C WR IT66121: 0x14 : 0x20200300
```
```
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x0180F300
I2C RD IT66121: 0x14 : 0x8080F300
I2C RD IT66121: 0x14 : 0x0C80F300
I2C RD IT66121: 0x14 : 0x2080F300
I2C RD IT66121: 0x14 : 0x8080F300
I2C RD IT66121: 0x14 : 0x3080F300
I2C RD IT66121: 0x14 : 0xE080F300
I2C RD IT66121: 0x14 : 0x2D80F300
I2C RD IT66121: 0x14 : 0x1080F300
I2C RD IT66121: 0x14 : 0x2880F300
I2C RD IT66121: 0x14 : 0x3080F300
```
Block 4
```
I2C RD IT66121: 0x10 : 0x2020A091
I2C RD IT66121: 0x10 : 0x2020A091
I2C WR IT66121: 0x10 : 0x2020A093
```
```
I2C RD IT66121: 0x14 : 0x3080F300
I2C WR IT66121: 0x14 : 0x30800F00
```
```
I2C RD IT66121: 0x10 : 0x2020A093
I2C RD IT66121: 0x10 : 0x2020A093
I2C WR IT66121: 0x10 : 0x2020A093
```
```
I2C RD IT66121: 0x14 : 0x3082FF00
I2C WR IT66121: 0x14 : 0x30820F00
```
```
I2C RD IT66121: 0x14 : 0x3082FF00
I2C WR IT66121: 0x14 : 0x30820900
```
```
I2C WR IT66121: 0x10 : 0x063DA001
```
```
I2C WR IT66121: 0x14 : 0x063D0300
```
```
I2C RD IT66121: 0x14 : 0xD380F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x6C80F300
```
Block 5
```
I2C RD IT66121: 0x10 : 0x063DA091
I2C RD IT66121: 0x10 : 0x063DA091
I2C WR IT66121: 0x10 : 0x063DA093
```
```
I2C RD IT66121: 0x14 : 0x0180F300
I2C WR IT66121: 0x14 : 0x01800F00
```
```
I2C RD IT66121: 0x10 : 0x063DA093
I2C RD IT66121: 0x10 : 0x063DA093
I2C WR IT66121: 0x10 : 0x063DA093
```
```
I2C RD IT66121: 0x14 : 0x0182FF00
I2C WR IT66121: 0x14 : 0x01820F00
```
```
I2C RD IT66121: 0x14 : 0x0182FF00
I2C WR IT66121: 0x14 : 0x01820900
```
```
I2C WR IT66121: 0x10 : 0x2040A001
```
```
I2C WR IT66121: 0x14 : 0x20400300
```
```
I2C RD IT66121: 0x14 : 0x4480F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x1880F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x1080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x1080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
```
Block 6
```
I2C RD IT66121: 0x10 : 0x2040A091
I2C RD IT66121: 0x10 : 0x2040A091
I2C WR IT66121: 0x10 : 0x2040A093
```
```
I2C RD IT66121: 0x14 : 0x0080F300
I2C WR IT66121: 0x14 : 0x00800F00
```
```
I2C RD IT66121: 0x10 : 0x2040A093
I2C RD IT66121: 0x10 : 0x2040A093
I2C WR IT66121: 0x10 : 0x2040A093
```
```
I2C RD IT66121: 0x14 : 0x0082FF00
I2C WR IT66121: 0x14 : 0x00820F00
```
```
I2C RD IT66121: 0x14 : 0x0082FF00
I2C WR IT66121: 0x14 : 0x00820900
```
```
I2C WR IT66121: 0x10 : 0x065DA001
```
```
I2C WR IT66121: 0x14 : 0x065D0300
```
```
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
```
Block 7
```
I2C RD IT66121: 0x10 : 0x065DA091
I2C RD IT66121: 0x10 : 0x065DA091
I2C WR IT66121: 0x10 : 0x065DA093
```
```
I2C RD IT66121: 0x14 : 0x0080F300
I2C WR IT66121: 0x14 : 0x00800F00
```
```
I2C RD IT66121: 0x10 : 0x065DA093
I2C RD IT66121: 0x10 : 0x065DA093
I2C WR IT66121: 0x10 : 0x065DA093
```
```
I2C RD IT66121: 0x14 : 0x0082FF00
I2C WR IT66121: 0x14 : 0x00820F00
```
```
I2C RD IT66121: 0x14 : 0x0082FF00
I2C WR IT66121: 0x14 : 0x00820900
```
```
I2C WR IT66121: 0x10 : 0x2060A001
```
```
I2C WR IT66121: 0x14 : 0x20600300
```
```
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x1080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x0080F300
I2C RD IT66121: 0x14 : 0x1780F300
```
**Configure FL2000 for streaming**

VGA_CTRL_REG_ACLK: Force PLL power UP always
```
REG RD 0x803C : 0xE001084D
REG WR 0x803C : 0xE401084D
```
**fl2000_monitor_set_resolution**
VGA_PLL_REG: configure PLL parameters
```
REG WR 0x802C : 0x0020410A
```
RST_CTRL_REG: Software reset to application logic
```
REG RD 0x8048 : 0x00000004
REG WR 0x8048 : 0x00008004
```
VGA_PLL_REG: confirm PLL parameters setting
```
REG RD 0x802C : 0x0020410A
```
VGA_CTRL_REG_ACLK: Do not use packet pending
```
REG RD 0x803C : 0xE401084D
REG WR 0x803C : 0xC401084D
```
VGA_CTRL_REG_ACLK: Use zero-length packet, VGA error interrupt enable
```
REG RD 0x803C : 0xC401084D
REG WR 0x803C : 0xD501084D
```
VGA_CTRL_REG_PXCLK: Disable DAC output, disable continuous drop count
```
REG RD 0x8004 : 0x0010239C
REG WR 0x8004 : 0x0010031C
```
VGA_CTRL_REG_PXCLK: Enable DAC output, Clear watermark
```
REG RD 0x8004 : 0x0010031C
REG WR 0x8004 : 0x0010039D
```
VGA_HSYNC_REG1: value
```
REG WR 0x8008 : 0x032003A0
REG RD 0x8008 : 0x032003A0
```
VGA_HSYNC_REG2: value
```
REG WR 0x800C : 0x00300059
REG RD 0x800C : 0x00300059
```
VGA_VSYNC_REG1: value
```
REG WR 0x8010 : 0x01E0020D
REG RD 0x8010 : 0x01E0020D
```
VGA_VSYNC_REG2: value
```
REG WR 0x8014 : 0x02130021
REG RD 0x8014 : 0x02130021
```
VGA_ISOCH_REG: reset mframe_count to 0
```
REG RD 0x801C : 0x00850000
REG WR 0x801C : 0x00000000
```
0x0070: magic set bit 13
```
REG RD 0x0070 : 0x04186085
REG WR 0x0070 : 0x04186085
```
**fl2000_hdmi_init (reordered)**
VGA_CTRL_REG_ACLK: Set "Force VGA Connect"
```
REG RD 0x803C : 0xD501084D
REG WR 0x803C : 0xD701084D
```
**IT66121 mute (fl2000_hdmi_av_mute)**

0x0F: Bank 0
```
I2C RD IT66121: 0x0C : 0x006C0000
I2C WR IT66121: 0x0C : 0x006C0000
```
0xC1: set AVMute
```
I2C RD IT66121: 0xC0 : 0x089A8102
I2C RD IT66121: 0xC0 : 0x089A8102
I2C WR IT66121: 0xC0 : 0x089A8102
```
0xC6: enable General Control Packet, Repeat General Control Packet
```
I2C RD IT66121: 0xC4 : 0xFF0004C0
I2C WR IT66121: 0xC4 : 0xFF0304C0
```
**IT66121 Set AVI InfoFrame (fl2000_hdmi_set_avi_info_frame)**

0x0F: Bank 1
```
I2C RD IT66121: 0x0C : 0x006C0000
I2C WR IT66121: 0x0C : 0x016C0000
```
0x158-0x15B: packed_frame 4..7
```
I2C WR IT66121: 0x58 : 0x00005810
```
0x15C-0x15F: packed_frame 8,3,9,10
```
I2C WR IT66121: 0x5C : 0x00000700
```
0x160-0x163: packed_frame 11..14
```
I2C WR IT66121: 0x60 : 0x00000000
```
0x164-0x165: packed_frame 15..16
```
I2C WR IT66121: 0x64 : 0x00000000
```
0x0F: Bank 0
```
I2C RD IT66121: 0x0C : 0x016C0000
I2C WR IT66121: 0x0C : 0x006C0000
```
0xCD: Enable AVI InfoFrame, Repeat AVI Infoframe
```
I2C RD IT66121: 0xCC : 0xFF0000FF
I2C WR IT66121: 0xCC : 0xFF0003FF
```
**fl2000_hdmi_enable_video_output**

0x04: set HDCP reset, clear Audio FIFO reset, clear SW Audio base clock reset
```
I2C RD IT66121: 0x04 : 0x0000601C
I2C WR IT66121: 0x04 : 0x00006009
```
**fl2000_hdmi_set_input_mode**

0x70: Input PCLK delay = 01
```
I2C RD IT66121: 0x70 : 0x10000800
I2C RD IT66121: 0x70 : 0x10000800
I2C WR IT66121: 0x70 : 0x10000801
```
**fl2000_hdmi_set_csc_scale**

0x0F: Power Down TxCLK
```
I2C RD IT66121: 0x0C : 0x006C0000
I2C RD IT66121: 0x0C : 0x006C0000
I2C WR IT66121: 0x0C : 0x106C0000
```
**fl2000_hdmi_set_csc_scale**

0x72: bypass dither, disable color conversion, Cr/Cb up/downsampling, noise filter
```
I2C RD IT66121: 0x70 : 0x10000801
I2C RD IT66121: 0x70 : 0x10000801
I2C WR IT66121: 0x70 : 0x10000801
```
0xC0: Set TX mode - HDMI
```
I2C RD IT66121: 0xC0 : 0x089A8102
I2C WR IT66121: 0xC0 : 0x089A8100
```
**IT66121 Setup AFE (according to guide) (fl2000_hdmi_setup_afe)**
0x61: Enable reset for HDMI_TX_DRV
```
I2C RD IT66121: 0x60 : 0x188810FF
I2C WR IT66121: 0x60 : 0x188810FF
```
0x62: set parameters for TDMS < 80MHz (guide: reset bit 7, set bit 4)
```
I2C RD IT66121: 0x60 : 0x188810FF
I2C RD IT66121: 0x60 : 0x188810FF
I2C WR IT66121: 0x60 : 0x181810FF
```
0x64: set parameters for TDMS < 80MHz guide: reset bit 7, set bits 3 and 1)
```
I2C RD IT66121: 0x64 : 0x00000094
I2C RD IT66121: 0x64 : 0x00000094
I2C WR IT66121: 0x64 : 0x0000001D
```
0x68: set parameters for TDMS < 80MHz (guide: set bit 4)
```
I2C RD IT66121: 0x68 : 0xFF003000
I2C RD IT66121: 0x68 : 0xFF003000
I2C WR IT66121: 0x68 : 0xFF003010
```
0x04: clear SW Video base clock reset
```
I2C RD IT66121: 0x04 : 0x00006009
I2C RD IT66121: 0x04 : 0x00006009
I2C WR IT66121: 0x04 : 0x00006001
```
0x61: Disable reset for HDMI_TX_DRV (guide: fire AFE by writing 0)
```
I2C RD IT66121: 0x60 : 0x181810FF
I2C WR IT66121: 0x60 : 0x181800FF
```
****
0x04: set HDCP reset
```
I2C RD IT66121: 0x04 : 0x00006001
I2C WR IT66121: 0x04 : 0x00006001
```
**fl2000_hdmi_setup_afe**
0x0F: Power up TxCLK, also by chance clear pending interrupt in bit 7
```
I2C RD IT66121: 0x0C : 0x106C0000
I2C WR IT66121: 0x0C : 0x006C0000
```
0x61: fire AFE by writing 0 (duplicate operation)
```
I2C RD IT66121: 0x60 : 0x181800FF
I2C WR IT66121: 0x60 : 0x181800FF
```
**IT66121 Start Audio Configuration (fl2000_hdmi_set_audio_info_frame)**
0x0F: Bank 1
```
I2C RD IT66121: 0x0C : 0x006C0000
I2C WR IT66121: 0x0C : 0x016C0000
```
0x168: Audio InfoFrame
```
I2C RD IT66121: 0x68 : 0x00000000
I2C WR IT66121: 0x68 : 0x00000001
I2C RD IT66121: 0x68 : 0x00000001
```
0x169: Audio InfoFrame
```
I2C RD IT66121: 0x68 : 0x00000001
I2C WR IT66121: 0x68 : 0x00000001
I2C RD IT66121: 0x68 : 0x00000001
```
0x16B: Audio InfoFrame
```
I2C RD IT66121: 0x68 : 0x00000001
I2C WR IT66121: 0x68 : 0x00000001
I2C RD IT66121: 0x68 : 0x00000001
```
0x16C: Audio InfoFrame
```
I2C RD IT66121: 0x6C : 0xFF7F7B00
I2C WR IT66121: 0x6C : 0xFF7F7B00
I2C RD IT66121: 0x6C : 0xFF7F7B00
```
0x16D: Audio InfoFrame checksum
```
I2C RD IT66121: 0x6C : 0xFF7F7B00
I2C WR IT66121: 0x6C : 0xFF7F7000
```
0x0F: Bank 0
```
I2C RD IT66121: 0x0C : 0x016C0000
I2C WR IT66121: 0x0C : 0x006C0000
```
0xCE: Enable Audio InfoFrame, Repeat Audio InfoFrame
```
I2C RD IT66121: 0xCC : 0xFF0003FF
I2C WR IT66121: 0xCC : 0xFF0303FF
```
**fl2000_hdmi_setup_audio_output**

0x04: Audio FIFO reset, SW Audio base clock reset
```
I2C RD IT66121: 0x04 : 0x00006001
I2C RD IT66121: 0x04 : 0x00006001
I2C WR IT66121: 0x04 : 0x00006015
```
0x58: MCLK 01: Auto oversampling clock, 2x128Fs
```
I2C RD IT66121: 0x58 : 0xFE030311
I2C WR IT66121: 0x58 : 0xFE030315
```
0x0F: reset bit 4
```
I2C RD IT66121: 0x0C : 0x006C0000
I2C RD IT66121: 0x0C : 0x006C0000
I2C WR IT66121: 0x0C : 0x006C0000
```
0xE0: Configure SPDIF
```
I2C RD IT66121: 0xE0 : 0x00E441C0
I2C RD IT66121: 0xE0 : 0x00E441C0
I2C WR IT66121: 0xE0 : 0x00E441C0
```
**IT66121 Program N / Clock TimeStamp (fl2000_hdmi_setup_ncts)**
0xE5: check bitrate
```
I2C RD IT66121: 0xE4 : 0x00000000
```
0x0F: Switch Bank 1
```
I2C RD IT66121: 0x0C : 0x006C0000
I2C WR IT66121: 0x0C : 0x016C0000
```
0x33: N0
```
I2C RD IT66121: 0x30 : 0x80F3D8A7
I2C WR IT66121: 0x30 : 0x00F3D8A7
```
0x34: N1
```
I2C RD IT66121: 0x34 : 0x00000018
I2C WR IT66121: 0x34 : 0x00000010
```
0x35: N2
```
I2C RD IT66121: 0x34 : 0x00000010
I2C WR IT66121: 0x34 : 0x00000010
```
0x30: CTS0
```
I2C RD IT66121: 0x30 : 0x00F3D8A7
I2C WR IT66121: 0x30 : 0x00F3D800
```
0x31: CTS1
```
I2C RD IT66121: 0x30 : 0x00F3D800
I2C WR IT66121: 0x30 : 0x00F30000
```
0x32: CTS2
```
I2C RD IT66121: 0x30 : 0x00F30000
I2C WR IT66121: 0x30 : 0x00000000
```
0x0F: Switch bank 0
```
I2C RD IT66121: 0x0C : 0x016C0000
I2C WR IT66121: 0x0C : 0x006C0000
```
N/CTS password byte 0<br>
0xF8: 00 -> C3
```
I2C RD IT66121: 0xF8 : 0xFFFFFF00
I2C WR IT66121: 0xF8 : 0xFFFFFFC3
```
N/CTS password byte 1<br>
0xF8: C3 -> A5
```
I2C RD IT66121: 0xF8 : 0xFFFFFFC3
I2C WR IT66121: 0xF8 : 0xFFFFFFA5
```
0xC5: Set HW auto count for CTS generation
```
I2C RD IT66121: 0xC4 : 0xFF0304C0
I2C RD IT66121: 0xC4 : 0xFF0304C0
I2C WR IT66121: 0xC4 : 0xFF0304C0
```
N/CTS protect back<br>
0xF8: A5 -> FF
```
I2C RD IT66121: 0xF8 : 0xFFFFFFA5
I2C WR IT66121: 0xF8 : 0xFFFFFFFF
```
Set HBR<br>
0x0F: Switch bank 1
```
I2C RD IT66121: 0x0C : 0x006C0000
I2C WR IT66121: 0x0C : 0x016C0000
```
0x198: Enable HBR (768kHz)
```
I2C RD IT66121: 0x98 : 0xFFFF7800
I2C WR IT66121: 0x98 : 0xFFFF7809
```
0x199: Enable HBR (768kHz)
```
I2C RD IT66121: 0x98 : 0xFFFF7809
I2C RD IT66121: 0x98 : 0xFFFF7809
I2C WR IT66121: 0x98 : 0xFFFF6809
```
0x0F: Switch bank 0
```
I2C RD IT66121: 0x0C : 0x016C0000
I2C WR IT66121: 0x0C : 0x006C0000
```
0x04: clear Audio FIFO reset
```
I2C RD IT66121: 0x04 : 0x00006015
I2C RD IT66121: 0x04 : 0x00006015
I2C WR IT66121: 0x04 : 0x00006011
```
**IT66121 Set Audio Channel Status (fl2000_hdmi_setup_ch_stat)**

0x0F: Switch to bank 1
```
I2C RD IT66121: 0x0C : 0x006C0000
I2C WR IT66121: 0x0C : 0x016C0000
```
0x90 - 0x93: ChStatData 0..2
```
I2C WR IT66121: 0x90 : 0x02000000
```
0x94: ChStatData 2
```
I2C RD IT66121: 0x94 : 0xFFFFFF06
I2C WR IT66121: 0x94 : 0xFFFFFF00
```
0x98 - 00x9B: ChStatData 3
```
I2C WR IT66121: 0x98 : 0x00000000
```
0x0F: Switch to bank 0
```
I2C RD IT66121: 0x0C : 0x016C0000
I2C WR IT66121: 0x0C : 0x006C0000
```
**IT66121 Configure Audio Format fl2000_hdmi_setup_pcm_audio**

0x0F: Switch bank 0
```
I2C RD IT66121: 0x0C : 0x006C0000
I2C WR IT66121: 0x0C : 0x006C0000
```
0xE0 - 0xE3: AudioFormat
```
I2C WR IT66121: 0xE0 : 0x00E44101
```
0xE4 - 0xE5: AudioFormat
```
I2C WR IT66121: 0xE4 : 0x00000000
```
0x09: Enable Audio overflow interrupt
```
I2C RD IT66121: 0x08 : 0xFFFFFF00
I2C RD IT66121: 0x08 : 0xFFFFFF00
I2C WR IT66121: 0x08 : 0xFFFF7F00
```
0x04: clear SW Audio base clock reset
```
I2C RD IT66121: 0x04 : 0x00006011
I2C RD IT66121: 0x04 : 0x00006011
I2C WR IT66121: 0x04 : 0x00006001
```
**IT66121 Unmute AV (fl2000_hdmi_av_mute)**

0x0F: Switch bank 0
```
I2C RD IT66121: 0x0C : 0x006C0000
I2C WR IT66121: 0x0C : 0x006C0000
```
0xC1: disable AVMute
```
I2C RD IT66121: 0xC0 : 0x089A8100
I2C RD IT66121: 0xC0 : 0x089A8100
I2C WR IT66121: 0xC0 : 0x089A8000
```
0xC6: enable General Control Packet, Repeat General Control Packet
```
I2C RD IT66121: 0xC4 : 0xFF0304C0
I2C WR IT66121: 0xC4 : 0xFF0304C0
```
**Strange sequence of NOT forcing "monitor connected" status**

*537 frames of 1152000 bytes each skipped*<br>
VGA_CTRL_REG_ACLK: Reset "Force VGA Connect"
```
REG RD 0x803C : 0xD701084D
REG WR 0x803C : 0xD501084D
```
*7 frames of 1152000 bytes each skipped*
```
INTERRUPT	1
REG RD 0x8000 : 0xA8087EE1
```
**Monitor disconnected**

*137 frames of 1152000 bytes each skipped*
```
INTERRUPT	1
REG RD 0x8000 : 0x480AA260
```
VGA_CTRL_REG_ACLK: VGA error interrupt disable
```
REG RD 0x803C : 0xD501084D
REG WR 0x803C : 0xD401084D
```
VGA_CTRL_REG_ACLK: allow hardware auto-control for PLL
```
REG RD 0x803C : 0xD401084D
REG WR 0x803C : 0xD001084D
```
**'Disable' sequence for USB interface**
```
REG RD 0x0078 : 0x18010D14
REG WR 0x0078 : 0x18030D14
```
```
REG RD 0x0070 : 0x04186085
REG WR 0x0070 : 0x04086085
```
```
REG RD 0x0070 : 0x04086085
REG WR 0x0070 : 0x04006085
```
