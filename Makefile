fl2000-y := \
	fl2000_drv.o \
	fl2000_registers.o \
	fl2000_interrupt.o \
	fl2000_streaming.o \
	fl2000_i2c.o \
	fl2000_connector.o \
	fl2000_drm.o

it66121-y := \
	bridge/it66121_drv.o

obj-m := fl2000.o it66121.o

# Add new architecture backends here; generic always retains the scalar path.
FL2000_FRAME_BACKEND ?= auto
ifneq ($(KERNELRELEASE),)
fl2000_frame_backend := $(FL2000_FRAME_BACKEND)
ifeq ($(fl2000_frame_backend),auto)
ifeq ($(CONFIG_X86_64),y)
fl2000_frame_backend := ssse3
else
fl2000_frame_backend := generic
endif
endif
ifeq ($(fl2000_frame_backend),ssse3)
ifneq ($(CONFIG_X86_64),y)
$(error SSSE3 frame backend requires an x86-64 target kernel)
endif
fl2000-y += frame/x86.o frame/ssse3.o
ccflags-y += -DFL2000_FRAME_ACCELERATED
CFLAGS_REMOVE_frame/ssse3.o += -mno-sse -mno-sse2
CFLAGS_frame/ssse3.o += -mssse3 -mno-avx
else ifeq ($(fl2000_frame_backend),bmi2)
ifneq ($(CONFIG_X86_64),y)
$(error BMI2 frame backend requires an x86-64 target kernel)
endif
fl2000-y += frame/x86_bmi2.o frame/bmi2.o
ccflags-y += -DFL2000_FRAME_ACCELERATED
CFLAGS_frame/bmi2.o += -mbmi2
else ifneq ($(fl2000_frame_backend),generic)
$(error Unknown FL2000_FRAME_BACKEND $(FL2000_FRAME_BACKEND))
endif
endif

KVER ?= $(shell uname -r)
KSRC ?= /lib/modules/$(KVER)/build

ccflags-y += -Wall

.PHONY: all modules clean

all:	modules

modules:
	$(MAKE) -C "$(KSRC)" M="$(CURDIR)" FL2000_FRAME_BACKEND=$(FL2000_FRAME_BACKEND) modules

clean:
	$(MAKE) -C "$(KSRC)" M="$(CURDIR)" clean
	$(RM) "$(CURDIR)/Module.symvers" "$(CURDIR)"/*.ur-safe
