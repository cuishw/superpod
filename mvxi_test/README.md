# mvxi_test

`mvxi_test` is a standalone GPU memory stress/fill utility. It loads the CUDA driver library at runtime, allocates as much free memory as possible on each GPU, and writes byte pattern `0xbb` to every allocated byte.

## Build

```sh
make
```

The program only needs a C compiler and `libdl` at build time. CUDA headers are not required because CUDA Driver API symbols are resolved dynamically from `libcuda.so` at runtime.

## Usage

```sh
./mvxi_test
./mvxi_test --device 0
./mvxi_test --reserve-mib 256
./mvxi_test --min-chunk-mib 4
```

Options:

- `--device <index>`: test only one GPU. By default every CUDA GPU is tested.
- `--reserve-mib <MiB>`: leave this much reported free memory unallocated. The default is `0`, so the tool attempts to allocate all currently free GPU memory.
- `--min-chunk-mib <MiB>`: smallest allocation chunk to keep trying after large allocations fail. The default is `1` MiB.

Allocated chunks are filled with `0xbb`, synchronized, reported, then freed before the program exits.
