# SPDX-License-Identifier: GPL-2.0
FL2000_FRAME_BACKEND ?= auto

KVER ?= $(shell uname -r)
KSRC ?= /lib/modules/$(KVER)/build

.PHONY: all modules clean

all:	modules

modules:
	$(MAKE) -C "$(KSRC)" M="$(CURDIR)" FL2000_FRAME_BACKEND=$(FL2000_FRAME_BACKEND) modules

clean:
	$(MAKE) -C "$(KSRC)" M="$(CURDIR)" clean
	$(RM) "$(CURDIR)/Module.symvers" "$(CURDIR)"/*.ur-safe
