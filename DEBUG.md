# Debugging the driver

## Prerequisites
 * [QEMU 2.12.0](https://www.qemu.org/download/)
 * [Virtme 0.0.3](https://github.com/amluto/virtme) also see nice [article](https://www.collabora.com/news-and-blog/blog/2018/09/18/virtme-the-kernel-developers-best-friend/) from Collabora
 * [Lua 5.2.4](https://www.lua.org/download.html)

## Running in a test environment
Debugging and development of kernel module may be an issue - crashes or resource leakage in kernelspace sometimes can be irreversible. To simplify host debugging I use qemu with virtme helper tools and couple of custom scripts:
 * scripts/emulate.sh - starts a vm with current host's kernel (so you need to build the driver using its headers) and passes to it FL2000 USB device access via VID:PID configuration
 * scripts/startup.sh - loads kernel modules and dependenceis for debugging/testing (execution can be automated with virtme in future)

## Register & I2C programming
Sequnce of register configuration and writing is not documented well for either FL2000 or IT66121. In both cases code implementations are rather basic and it is very hard to simply copy-paste them into "clean" implementation. This is also true for DDC I2C communication for EDID processing or IT66121 modes configuration or frames streaming. On the other hand, Windows driver for FL2000DRM based dongle is fully available and seem to be working properly: connected display resolution recognized, Windows desktop can be seen, no artifacts present, etc. This is true for VGA and HDMI dongle versions, which means that programming for both FL2000 and IT66121 can be studied. This makes possible implementation of HW related stuff via reverse-engineering: dump USB bus interactions, parse them with Wireshark or similar tool, formalize logic and implement it.

Since there is lots of unknowns, it could be beneficial to create a flexible system that would allow userspace register programming without the need to rebuild the driver and restart the VM; when programming model and flow is clarified, implementation can be moved from userspace to the driver modules. With this approach, first implementation is done in userspace using simple Lua script and driver-exposed custom debugfs entries for accessing registers and I2C and interrupts status values ringbuffer, as well as sending frame buffer stream.

**Register programming** `/sys/kernel/debug/fl2000_regs`
- `reg_address`: specify register address to work with
- `reg_data`: read causes read from register, write causes write to register

**I2C access** `/sys/kernel/debug/fl2000_i2c`
- `i2c_address`: i2c device address to operate with
- `i2c_offset`: specify device register address to work with
- `i2c_value`: read causes i2c read, write causes i2c write

**Interrupt handling** `/sys/kernel/debug/fl2000_interrupt`
- `intr_status`: array of values of interrupt statuses since last reading

Following functions are already implemented in the driver code:
* interrupts status register processing
* automatic IT66121 client detection on I2C bus

*TODO* Add buffer for sending arbitrary frame without using DRM

Dump of correctly working windows driver with non-standard HDMI display is [available on Dropbox](https://www.dropbox.com/s/niizuk2d4xrkcmg/fl2000.pcapng?dl=0) (45MB pcapng)

## DRM implementation
*TODO* Support for DRM development tools
