#!/bin/bash

# Intel or AMD IOMMU driver for qemu
IOMMU="intel-iommu"

# USB Host Controller PCI bus enumeration bus ID
#  - 0000:04:00.0 for ASMedia USB Controller
#  - 0000:00:14.0 for Intel USB controller
USB_ID="0000:00:14.0"

# Disconnect USB Host Controller from xHCI driver using its PCI bus ID
if [[ -d "/sys/bus/pci/drivers/xhci_hcd/$USB_ID" ]]; then
	echo -n $USB_ID | sudo tee /sys/bus/pci/drivers/xhci_hcd/unbind
fi

# Start virtme
# NOTE: There is a bug in qemu that does not properly allow using together
# kernel_irqchip=split and intremao=on. There is work being done to re-enable
# fastpath for INTx but it is not merged yet as of now
virtme-run --pwd --installed-kernel --qemu-opts -cpu host \
	-m 4G \
	-nic user,ipv6=off,model=e1000 \
	-machine q35,accel=kvm,kernel_irqchip=split \
	-device $IOMMU,intremap=on,caching-mode=on,device-iotlb=on \
	-device vfio-pci,host=$USB_ID -s
