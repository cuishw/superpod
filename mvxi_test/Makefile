# SPDX-License-Identifier: MIT
CC ?= cc
CFLAGS ?= -Wall -Wextra -O2
LDLIBS ?= -ldl

.PHONY: all clean install

all: mvxi_test

mvxi_test: mvxi_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	$(RM) mvxi_test

install: mvxi_test
	install -m 0755 mvxi_test /usr/local/bin/mvxi_test
