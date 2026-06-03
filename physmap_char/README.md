# physmap_char

`physmap_char` provides a small Linux kernel module plus a userspace configuration tool for exposing selected physical address ranges through mmap-able character devices.

## What it does

1. Loading the kernel module creates `/dev/physmap_ctl`.
2. `physmapctl create <identifier> <phys_addr> <size> [UC|WC|WB]` asks the driver to register a uniquely identified mapping.
3. The driver creates `/dev/physmapN` and returns that path to userspace.
4. Other applications open `/dev/physmapN` and call `mmap(2)` to access the requested physical range.

Cache mode defaults to write-combining (`WC`) when omitted. `UC`, `WC`, and `WB` are supported.

> **Safety note:** this driver intentionally maps physical memory into userspace. Use only on trusted systems and restrict permissions on the generated device nodes with udev or equivalent policy.

## Build

```sh
make
```

The build expects kernel headers for the running kernel at `/lib/modules/$(uname -r)/build`. Override with `KDIR=/path/to/kernel/build` if needed.

## Install and load

```sh
sudo insmod physmap_driver.ko
sudo install -m 0755 physmapctl /usr/local/sbin/physmapctl
sudo install -m 0755 physmap_memtest /usr/local/sbin/physmap_memtest
```

## Create a mapping

```sh
sudo physmapctl create framebuffer0 0x80000000 0x100000
```

Example output:

```text
id=0
identifier=framebuffer0
dev=/dev/physmap0
```

To select a cache mode explicitly:

```sh
sudo physmapctl create framebuffer_uc 0x80000000 0x100000 UC
sudo physmapctl create framebuffer_wc 0x80000000 0x100000 WC
sudo physmapctl create framebuffer_wb 0x80000000 0x100000 WB
```

The identifier must be unique among active mappings. Both physical address and size must be page-aligned. Numeric values accept decimal or `0x`-prefixed hexadecimal input and optional binary size suffixes (`K`, `M`, `G`, `T`, `P`, with optional `B`/`iB`), so `64G` is accepted as 64 GiB.

## Use from another application

```c
int fd = open("/dev/physmap0", O_RDWR | O_SYNC);
void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
```

The mmap offset can be used to map a page-aligned subrange inside the configured physical range.

## Standalone memory read/write test tool

`physmap_memtest` is a separate mmap test utility modeled after simple reserved-memory test programs. It defaults to `/dev/physmap0`, or you can pass another mapped character device as the first argument.

```sh
sudo physmap_memtest read 0x0 64
sudo physmap_memtest /dev/physmap1 read 0x1000 256
sudo physmap_memtest write 0x0 64 0x5a
sudo physmap_memtest read64 0x1000
sudo physmap_memtest write64 0x1000 0xdeadbeef12345678
```

The tool maps a page-aligned window that covers the requested offset and size, so command offsets do not need to be page-aligned. The `read64` and `write64` commands require 8-byte-aligned offsets.

## List mappings

```sh
sudo physmapctl list
```

Example output:

```text
ID   IDENTIFIER           DEV              PHYS_ADDR          SIZE               END_ADDR           CACHE  REFS
0    framebuffer0         /dev/physmap0    0x0000000080000000 0x0000000000100000 0x00000000800fffff WC     1
```

The list command shows every currently configured character device, including mapping id, unique identifier, device path, physical base address, size, end address, cache mode, and active driver reference count.

## Destroy a mapping

```sh
sudo physmapctl destroy 0
```

Destroying a mapping removes the device node. Existing open file descriptors remain valid until closed.
