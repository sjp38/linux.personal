KERNELDIR = /lib/modules/$(shell uname -r)/build

obj-m += static_cp_module.o
delayed-objs := static_cp_module.c

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	rm *.mod.*
	rm *.ko
	rm *.o
