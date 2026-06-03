# physmap_char

`physmap_char` provides a small Linux kernel module plus a userspace configuration tool for exposing selected physical address ranges through mmap-able character devices.

## What it does

1. Loading the kernel module creates `/dev/physmap_ctl`.
2. `physmapctl create <phys_addr> <size> [UC|WC|WB]` asks the driver to register a mapping.
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
```

## Create a mapping

```sh
sudo physmapctl create 0x80000000 0x100000
```

Example output:

```text
id=0
dev=/dev/physmap0
```

To select a cache mode explicitly:

```sh
sudo physmapctl create 0x80000000 0x100000 UC
sudo physmapctl create 0x80000000 0x100000 WC
sudo physmapctl create 0x80000000 0x100000 WB
```

Both physical address and size must be page-aligned. Numeric values accept decimal or `0x`-prefixed hexadecimal input and optional binary size suffixes (`K`, `M`, `G`, `T`, `P`, with optional `B`/`iB`), so `64G` is accepted as 64 GiB.

## Use from another application

```c
int fd = open("/dev/physmap0", O_RDWR | O_SYNC);
void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
```

The mmap offset can be used to map a page-aligned subrange inside the configured physical range.

## Destroy a mapping

```sh
sudo physmapctl destroy 0
```

Destroying a mapping removes the device node. Existing open file descriptors remain valid until closed.
