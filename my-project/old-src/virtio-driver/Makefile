obj-m += virtio_balloon.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(shell pwd) modules

clean:
	make -C $(KDIR) M=$(shell pwd) clean

install:
	make -C $(KDIR) M=$(shell pwd) modules_install
	depmod -a