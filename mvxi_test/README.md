# mvxi_test

`mvxi_test` is a standalone Muxi/MUSA GPU memory fill utility. It uses the MUSA runtime API from `mc_runtime.h`, allocates as much free GPU memory as possible, and writes byte pattern `0xbb` to every allocated byte.

## Build

```sh
make
```

The default compiler is `mxcc`.

## Usage

```sh
./mvxi_test --all
./mvxi_test --gpu 0
./mvxi_test --gpu 0 --reserve-mib 256 --min-chunk-mib 4
```

Options:

- `--all`: test every MUSA GPU. This is the default.
- `--gpu <ID>`: test only one GPU.
- `--reserve-mib <MiB>`: leave this much reported free memory unallocated. The default is `0`, so the tool attempts to allocate all currently free GPU memory.
- `--min-chunk-mib <MiB>`: smallest allocation chunk to keep trying after large allocations fail. The default is `1` MiB.

Allocated chunks are filled with `0xbb`, synchronized with `mcDeviceSynchronize()`, reported, then freed before the program exits.
