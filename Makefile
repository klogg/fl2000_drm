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

# Install enumeration independently of the external kernel modules.
.PHONY: install-enumerator
install-enumerator:
	install -Dm755 userspace/fl2000-enumerate.sh "$(DESTDIR)/usr/libexec/fl2000-enumerate"
	install -Dm644 userspace/fl2000-enumerate@.service "$(DESTDIR)/usr/lib/systemd/system/fl2000-enumerate@.service"
	install -Dm644 userspace/70-fl2000.rules "$(DESTDIR)/usr/lib/udev/rules.d/70-fl2000.rules"
	@if [ ! -e "$(DESTDIR)/etc/fl2000/enumerator.conf" ]; then \
		install -Dm644 userspace/enumerator.conf "$(DESTDIR)/etc/fl2000/enumerator.conf"; \
	fi
