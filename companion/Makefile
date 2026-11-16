KERNEL_RELEASE ?= $(shell uname -r)
KERNEL_DIR ?= /lib/modules/$(KERNEL_RELEASE)/build
obj-m += lotspeed_dup.o
ccflags-y += -std=gnu99

.PHONY: all clean
all:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) clean
