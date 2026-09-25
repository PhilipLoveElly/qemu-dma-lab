obj-m += philip_dma_drv.o
ccflags-y += -I$(src)/../include

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)


all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules


clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
