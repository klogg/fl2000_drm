#!/bin/sh

# Intel or AMD IOMMU driver for qemu
IOMMU="intel-iommu"

# Here is ASMedia Technology Inc. ASM1142 USB 3.1 Host Controller configuration
# detais - PCI Vendor:Device IDs and USB enumeration bus ID
PCI_ID="1b21:1242"
USB_ID="0000:04:00.0"

# Disconnect USB controller from xHCI driver
echo $USB_ID | sudo tee /sys/bus/pci/drivers/xhci_hcd/unbind

# Start vfio
modprobe vfio
modprobe vfio_iommu_type1
modprobe vfio_pci ids=$PCI_ID
modprobe vfio_virqfd

# Start virtme
virtme-run --pwd --installed-kernel --qemu-opts -cpu host \
	-nic user,ipv6=off,model=e1000 \
	-machine q35,accel=kvm,kernel_irqchip=split \
	-device $IOMMU,intremap=on,caching-mode=on \
	-device vfio-pci,host=$USB_ID -s
