#!/bin/sh

#dhclient

#modprobe usbmon

modprobe drm
modprobe drm_kms_helper

#tcpdump -i usbmon2 -nn -w /tmp/usb.pcap -c 20000 &

insmod fl2000.ko
insmod it66121.ko i2c_bus_num=1
