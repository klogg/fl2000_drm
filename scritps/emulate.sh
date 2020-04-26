#!/bin/sh

# Intel or AMD IOMMU driver for qemu
IOMMU="intel-iommu"

# USB Host Controller PCI bus enumeration bus ID
USB_ID="0000:04:00.0"

# Disconnect USB Host Controller from xHCI driver using its PCI bus ID
if [[ -d "/sys/bus/pci/drivers/xhci_hcd/$USB_ID" ]]; then
	echo -n $USB_ID | sudo tee /sys/bus/pci/drivers/xhci_hcd/unbind
fi

# Start virtme
virtme-run --pwd --installed-kernel --qemu-opts -cpu host \
	-nic user,ipv6=off,model=e1000 \
	-machine q35,accel=kvm,kernel_irqchip=split \
	-device $IOMMU,intremap=on,caching-mode=on \
	-device vfio-pci,host=$USB_ID -s
