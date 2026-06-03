# SPDX-License-Identifier: GPL-2.0
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
CC ?= cc
CFLAGS ?= -Wall -Wextra -O2

obj-m += physmap_driver.o

.PHONY: all modules userspace clean install

all: modules userspace

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

userspace: physmapctl

physmapctl: physmapctl.c physmap_ioctl.h
	$(CC) $(CFLAGS) -o $@ physmapctl.c

clean:
	$(RM) physmapctl
	@if [ -d "$(KDIR)" ]; then \
		$(MAKE) -C $(KDIR) M=$(PWD) clean; \
	else \
		echo "Skipping kernel clean: $(KDIR) not found"; \
	fi

install: all
	install -m 0755 physmapctl /usr/local/sbin/physmapctl
