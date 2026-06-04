# SPDX-License-Identifier: MIT
CC = mxcc
CFLAGS ?= -Wall -Wextra -O2
LDLIBS ?=

.PHONY: all clean install

all: data_validation

data_validation: data_validation.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	$(RM) data_validation

install: data_validation
	install -m 0755 data_validation /usr/local/bin/data_validation
