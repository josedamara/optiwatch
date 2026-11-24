# Out-of-tree build of the emulated transceiver I2C adapter.
#   make            build xcvr_i2c_emu.ko against the running kernel
#   make KDIR=...   build against another kernel tree
#   sudo insmod xcvr_i2c_emu.ko nr_cages=4

obj-m := xcvr_i2c_emu.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
