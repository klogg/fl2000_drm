#!/bin/sh

virtme-run --pwd --installed-kernel --qemu-opts -cpu host -accel kvm -device qemu-xhci -device usb-host,vendorid=0x1d5c,productid=0x2000 -s
