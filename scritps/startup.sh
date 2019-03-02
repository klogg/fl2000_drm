#!/bin/sh

modprobe drm
modprobe drm_kms_helper

insmod fl2000.ko
insmod it66121.ko

### Now lets debug registers initialization ###

# Start some clocks
# regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, 0x1F, 0x1C);


# Reject USB U1/U2 transitions
# regmap_write_bits(regmap, FL2000_USB_LPM, (3<<19), (3<<19));
# regmap_write_bits(regmap, FL2000_USB_LPM, (3<<20), (3<<20));


# Enable wakeup auto reset
# regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_3, (1<<10), (1<<10));

