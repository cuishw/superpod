# SPDX-License-Identifier: GPL-2.0
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
CC ?= cc
MXCC ?= mxcc
CFLAGS ?= -Wall -Wextra -O2

obj-m += physmap_driver.o

.PHONY: all modules userspace musa clean install install-musa

all: modules userspace

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

userspace: physmapctl physmap_memtest

physmapctl: physmapctl.c physmap_ioctl.h
	$(CC) $(CFLAGS) -o $@ physmapctl.c

physmap_memtest: physmap_memtest.c physmap_ioctl.h
	$(CC) $(CFLAGS) -o $@ physmap_memtest.c

musa: physmap_mc_memtest physmap_mc_bwtest

physmap_mc_memtest: physmap_mc_memtest.c physmap_ioctl.h
	$(MXCC) $(CFLAGS) -o $@ physmap_mc_memtest.c

physmap_mc_bwtest: physmap_mc_bwtest.c
	$(MXCC) $(CFLAGS) -o $@ physmap_mc_bwtest.c

clean:
	$(RM) physmapctl physmap_memtest physmap_mc_memtest physmap_mc_bwtest
	@if [ -d "$(KDIR)" ]; then \
		$(MAKE) -C $(KDIR) M=$(PWD) clean; \
	else \
		echo "Skipping kernel clean: $(KDIR) not found"; \
	fi

install: all
	install -m 0755 physmapctl /usr/local/sbin/physmapctl
	install -m 0755 physmap_memtest /usr/local/sbin/physmap_memtest

install-musa: physmap_mc_memtest physmap_mc_bwtest
	install -m 0755 physmap_mc_memtest /usr/local/sbin/physmap_mc_memtest
	install -m 0755 physmap_mc_bwtest /usr/local/sbin/physmap_mc_bwtest
