KERNELDIR = /lib/modules/$(shell uname -r)/build

obj-m += prcp_module.o  
delayed-objs := prcp_module.c 
#obj-m += cache_flooding_calculix.o  
#delayed-objs := cache_flooding_calculix.c 


KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	rm *.mod.*
	rm *.ko
	rm *.o
