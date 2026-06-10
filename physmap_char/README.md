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

`physmap_memtest` is a separate mmap test utility modeled after simple reserved-memory test programs. It defaults to `/dev/physmap0`, or you can pass another mapped character device as the first argument. The device argument accepts either a full `/dev/...` path or a physmap `IDENTIFIER`, which is resolved through `/dev/physmap_ctl`.

```sh
sudo physmap_memtest read 0x0 64
sudo physmap_memtest /dev/physmap1 read 0x1000 256
sudo physmap_memtest h0-0 read 0x1000 256
sudo physmap_memtest write 0x0 64 0x5a
sudo physmap_memtest read64 0x1000
sudo physmap_memtest write64 0x1000 0xdeadbeef12345678
```

The tool maps a page-aligned window that covers the requested offset and size, so command offsets do not need to be page-aligned. The `read64` and `write64` commands require 8-byte-aligned offsets.

## Muxi GPU read/write test tool

`physmap_mc_memtest` uses the MUSA runtime (`mc_runtime.h`) to access a mapped physical range through the GPU/DMA path instead of CPU loads and stores. Build it on a Muxi SDK system with:

```sh
make musa
sudo install -m 0755 physmap_mc_memtest /usr/local/sbin/physmap_mc_memtest
```

It defaults to GPU 0, `/dev/physmap0`, and `mcHostRegisterIoMemory` because physmap mappings usually represent I/O or physical memory. Like `physmap_memtest`, its device argument accepts either a full `/dev/...` path or a physmap `IDENTIFIER` resolved through `/dev/physmap_ctl`.

```sh
sudo physmap_mc_memtest --gpu 0 /dev/physmap0 write 0x0 64 0xbb
sudo physmap_mc_memtest --gpu 0 /dev/physmap0 read 0x0 64
sudo physmap_mc_memtest --gpu 0 h0-0 read 0x0 64
sudo physmap_mc_memtest --gpu 0 /dev/physmap0 write64 0x1000 0xdeadbeef12345678
sudo physmap_mc_memtest --gpu 0 /dev/physmap0 read64 0x1000
```

Use `--register default` instead of the default `--register io` if the mapped range should be registered with `mcHostRegisterDefault`.

## Muxi GPU bandwidth test tool

`physmap_mc_bwtest` measures MUSA DMA bandwidth for host memory that is either allocated by `mcMallocHost` or mapped with `mmap` and registered with `mcHostRegister`. For physmap character devices, use `--host mmap-register --mode file --path /dev/physmapN --io`. The tool validates GPU write/read consistency with the selected byte value before measuring bandwidth, then reports read bandwidth first and write bandwidth second.

```sh
make musa
sudo physmap_mc_bwtest --host mmap-register --mode file --path /dev/physmap0 --size 64 --offset 0 --iters 100 --gpu 0 --io --value 0xa5
sudo physmap_mc_bwtest --host mmap-register --mode file --path /sys/bus/pci/devices/0000:86:00.0/resource2 --size 1024 --offset 2G --iters 100 --gpu 0 --io
sudo physmap_mc_bwtest --host malloc-host --size 1024 --iters 100 --gpu 0
```

Options:

* `--host mmap-register|malloc-host`: use `mmap + mcHostRegister` (default) or `mcMallocHost` pinned memory.
* `--mode anon|file`: for `mmap-register`, map anonymous memory (default) or a file/BAR/device path.
* `--path PATH`: file/BAR/device path required by `--mode file`.
* `--size MB`: transfer size in MiB, default `1024`.
* `--offset BYTES`: page-aligned mmap offset for `--mode file`, default `0`; accepts decimal, `0x` hexadecimal, and binary suffixes such as `4K` or `2G`.
* `--iters N`: read and write iterations, default `100`.
* `--gpu ID`: GPU device id, default `0`.
* `--io`: register mapped memory with `mcHostRegisterIoMemory` instead of `mcHostRegisterDefault`.
* `--value BYTE` / `--data BYTE`: byte value used for validation and write-bandwidth traffic, default `0xa5`.

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
