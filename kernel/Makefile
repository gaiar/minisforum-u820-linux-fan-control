obj-m += u820_ec_probe.o
KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) W=1 modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
