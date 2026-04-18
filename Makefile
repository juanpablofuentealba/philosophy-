# Makefile for SmartlinkTechnology M01 Linux Kernel Driver
#
# Copyright (C) 2024 SmartlinkTechnology
#

obj-m += smartlink_m01.o

# Kernel build directory (default to current running kernel)
KDIR ?= /lib/modules/$(shell uname -r)/build

# Current directory
PWD := $(shell pwd)

# Debug level (0=none, 1=info, 2=debug)
DEBUG ?= 0

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f *.o *.ko *.mod.c *.mod.o *.symvers *.order .*.cmd

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

load:
	insmod smartlink_m01.ko debug=$(DEBUG)

unload:
	rmmod smartlink_m01

reload: unload load

# Build with debug enabled
debug:
	$(MAKE) DEBUG=1 all

# Check kernel headers
check:
	@echo "Checking kernel headers..."
	@if [ ! -d "$(KDIR)" ]; then \
		echo "Error: Kernel headers not found at $(KDIR)"; \
		echo "Please install kernel headers: sudo apt-get install linux-headers-$(shell uname -r)"; \
		exit 1; \
	else \
		echo "Kernel headers found at $(KDIR)"; \
	fi

# Show module info
info:
	modinfo smartlink_m01.ko

# Load module and create device node
start: load
	@mknod /dev/m01 c $$(cat /proc/devices | grep smartlink_m01 | awk '{print $$1}') 0 2>/dev/null || true
	@chmod 666 /dev/m01 2>/dev/null || true
	@echo "Module loaded. Device node created at /dev/m01"

# Unload module and remove device node
stop: unload
	@rm -f /dev/m01
	@echo "Module unloaded. Device node removed."

.PHONY: all clean install load unload reload debug check info start stop
