fl2000-y := \
		fl2000_module.o \
		fl2000_registers.o \
		fl2000_interrupt.o \
		fl2000_streaming.o \
		fl2000_i2c.o \

obj-m := fl2000.o

KSRC = /lib/modules/$(shell uname -r)/build

all:	modules

modules:
	make -C $(KSRC) M=$(PWD) modules

clean:
	make -C $(KSRC) M=$(PWD) clean
	rm -f Module.symvers
