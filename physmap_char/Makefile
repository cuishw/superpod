# SPDX-License-Identifier: GPL-2.0
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
CC ?= cc
MXCC ?= mxcc
CFLAGS ?= -Wall -Wextra -O2
MUSA_HOME ?= /usr/local/musa
MUSA_CPPFLAGS ?= -I$(MUSA_HOME)/include
MUSA_LIBDIR ?= $(MUSA_HOME)/lib
MUSA_LDFLAGS ?= -L$(MUSA_LIBDIR)
MUSA_LDLIBS ?= -ldl

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

physmap_mc_bwtest.o: physmap_mc_bwtest.c physmap_ioctl.h
	$(MXCC) $(CFLAGS) $(MUSA_CPPFLAGS) \
		-DDEFAULT_MUSA_LIBDIR=\"$(MUSA_LIBDIR)\" -c -o $@ physmap_mc_bwtest.c

physmap_mc_bwtest: physmap_mc_bwtest.o
	$(CC) $(CFLAGS) -o $@ physmap_mc_bwtest.o \
		$(MUSA_LDFLAGS) $(MUSA_LDLIBS)

clean:
	$(RM) physmapctl physmap_memtest physmap_mc_memtest physmap_mc_bwtest \
		physmap_mc_bwtest.o
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
