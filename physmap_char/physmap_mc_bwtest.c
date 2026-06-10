// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <mc_runtime.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "physmap_ioctl.h"

#define DEFAULT_CTL_DEV "/dev/physmap_ctl"
#define DEFAULT_DEV_PATH "/dev/physmap0"
#define DEFAULT_PAGE_SIZE 4096UL
#define DEFAULT_SIZE_MB 64ULL
#define DEFAULT_ITERS 100ULL
#define DEFAULT_WRITE_VALUE 0xa5U
#ifndef DEFAULT_MUSA_LIBDIR
#define DEFAULT_MUSA_LIBDIR "/usr/local/musa/lib"
#endif
#define DEFAULT_MC_RUNTIME_ENV "PHYSMAP_MC_RUNTIME"

#define CHECK_MC(call)                                                       \
	do {                                                                     \
		mcError_t err__ = (call);                                           \
		if (err__ != mcSuccess) {                                          \
			fprintf(stderr, "MC error %s:%d: %s\n", __FILE__, __LINE__,  \
				mcGetErrorString(err__));                                \
			exit(EXIT_FAILURE);                                        \
		}                                                                  \
	} while (0)

struct options {
	uint64_t size;
	uint64_t offset;
	uint64_t iters;
	uint8_t write_value;
	int gpu;
};

struct mc_runtime_api {
	void *handle;
	const char *(*mcGetErrorString)(mcError_t error);
	mcError_t (*mcSetDevice)(int device);
	mcError_t (*mcHostRegister)(void *ptr, size_t size, unsigned int flags);
	mcError_t (*mcHostUnregister)(void *ptr);
	mcError_t (*mcMalloc)(void **dev_ptr, size_t size);
	mcError_t (*mcFree)(void *dev_ptr);
	mcError_t (*mcMemset)(void *dev_ptr, int value, size_t count);
	mcError_t (*mcMemcpy)(void *dst, const void *src, size_t count,
			    int kind);
	mcError_t (*mcDeviceSynchronize)(void);
};

static struct mc_runtime_api mc_api;

static void load_mc_symbol(void *handle, void *fn_ptr, const char *name)
{
	dlerror();
	*(void **)fn_ptr = dlsym(handle, name);
	if (!*(void **)fn_ptr) {
		const char *err = dlerror();

		fprintf(stderr, "dlsym %s failed: %s\n", name,
			err ? err : "symbol not found");
		exit(EXIT_FAILURE);
	}
}

static void load_mc_runtime(void)
{
	const char *env_path = getenv(DEFAULT_MC_RUNTIME_ENV);
	char default_path[PATH_MAX];
	char default_path_v1[PATH_MAX];
	char default_path_v0[PATH_MAX];
	const char *candidates[10];
	int idx = 0;
	const char *last_error = NULL;

	if (mc_api.handle)
		return;

	if (env_path && *env_path)
		candidates[idx++] = env_path;
	candidates[idx++] = "libmusart.so";
	candidates[idx++] = "libmusart.so.1";
	candidates[idx++] = "libmusart.so.0";
	if (snprintf(default_path, sizeof(default_path), "%s/libmusart.so",
		     DEFAULT_MUSA_LIBDIR) < (int)sizeof(default_path))
		candidates[idx++] = default_path;
	if (snprintf(default_path_v1, sizeof(default_path_v1), "%s/libmusart.so.1",
		     DEFAULT_MUSA_LIBDIR) < (int)sizeof(default_path_v1))
		candidates[idx++] = default_path_v1;
	if (snprintf(default_path_v0, sizeof(default_path_v0), "%s/libmusart.so.0",
		     DEFAULT_MUSA_LIBDIR) < (int)sizeof(default_path_v0))
		candidates[idx++] = default_path_v0;
	candidates[idx] = NULL;

	for (int i = 0; candidates[i]; i++) {
		mc_api.handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
		if (mc_api.handle)
			break;
		last_error = dlerror();
	}
	if (!mc_api.handle) {
		fprintf(stderr,
			"failed to load MUSA runtime library. Set %s to the full libmusart.so path. Last error: %s\n",
			DEFAULT_MC_RUNTIME_ENV,
			last_error ? last_error : "unknown dlopen error");
		exit(EXIT_FAILURE);
	}

	load_mc_symbol(mc_api.handle, &mc_api.mcGetErrorString, "mcGetErrorString");
	load_mc_symbol(mc_api.handle, &mc_api.mcSetDevice, "mcSetDevice");
	load_mc_symbol(mc_api.handle, &mc_api.mcHostRegister, "mcHostRegister");
	load_mc_symbol(mc_api.handle, &mc_api.mcHostUnregister, "mcHostUnregister");
	load_mc_symbol(mc_api.handle, &mc_api.mcMalloc, "mcMalloc");
	load_mc_symbol(mc_api.handle, &mc_api.mcFree, "mcFree");
	load_mc_symbol(mc_api.handle, &mc_api.mcMemset, "mcMemset");
	load_mc_symbol(mc_api.handle, &mc_api.mcMemcpy, "mcMemcpy");
	load_mc_symbol(mc_api.handle, &mc_api.mcDeviceSynchronize,
		       "mcDeviceSynchronize");
}

#define mcGetErrorString mc_api.mcGetErrorString
#define mcSetDevice mc_api.mcSetDevice
#define mcHostRegister mc_api.mcHostRegister
#define mcHostUnregister mc_api.mcHostUnregister
#define mcMalloc mc_api.mcMalloc
#define mcFree mc_api.mcFree
#define mcMemset mc_api.mcMemset
#define mcMemcpy mc_api.mcMemcpy
#define mcDeviceSynchronize mc_api.mcDeviceSynchronize

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s [--size MB] [--offset BYTES] [--iters N] [--gpu ID] "
		"[--value BYTE|--data BYTE] [device]\n"
		"\n"
		"Defaults: --size %llu, --offset 0, --iters %llu, --gpu 0, "
		"--value/--data 0x%02x, device %s.\n"
		"Device may be a full /dev/... path or a physmap IDENTIFIER resolved "
		"through %s.\n"
		"--size is in MiB; --offset accepts decimal, 0x-prefixed hexadecimal, "
		"and K/M/G/T/P binary suffixes.\n",
		prog, (unsigned long long)DEFAULT_SIZE_MB,
		(unsigned long long)DEFAULT_ITERS, DEFAULT_WRITE_VALUE,
		DEFAULT_DEV_PATH, DEFAULT_CTL_DEV);
}

static const char *resolve_dev_path(const char *device, char *path_buf,
				    size_t path_buf_len)
{
	struct physmap_list_req req = { 0 };
	uint32_t count;
	int fd;

	if (!device || !*device)
		return DEFAULT_DEV_PATH;
	if (device[0] == '/')
		return device;

	fd = open(DEFAULT_CTL_DEV, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s failed while resolving identifier %s: %s\n",
			DEFAULT_CTL_DEV, device, strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (ioctl(fd, PHYSMAP_IOC_LIST, &req) < 0) {
		fprintf(stderr, "PHYSMAP_IOC_LIST failed while resolving identifier %s: %s\n",
			device, strerror(errno));
		close(fd);
		exit(EXIT_FAILURE);
	}
	close(fd);

	count = req.count > PHYSMAP_MAX_MAPPINGS ? PHYSMAP_MAX_MAPPINGS : req.count;
	for (uint32_t i = 0; i < count; i++) {
		if (!strcmp(req.entries[i].identifier, device)) {
			int written = snprintf(path_buf, path_buf_len, "%s",
					       req.entries[i].dev_name);

			if (written < 0 || (size_t)written >= path_buf_len) {
				fprintf(stderr, "device path for identifier %s is too long\n",
					device);
				exit(EXIT_FAILURE);
			}
			return path_buf;
		}
	}

	fprintf(stderr, "identifier not found: %s\n", device);
	exit(EXIT_FAILURE);
}

static int scale_for_suffix(const char *suffix, uint64_t *scale)
{
	char unit;

	if (!suffix || !*suffix) {
		*scale = 1;
		return 0;
	}

	unit = suffix[0];
	if (suffix[1] == 'i' || suffix[1] == 'I') {
		if (suffix[2] && (suffix[2] != 'b' && suffix[2] != 'B'))
			return -1;
		if (suffix[2] && suffix[3])
			return -1;
	} else if (suffix[1] && (suffix[1] != 'b' && suffix[1] != 'B')) {
		return -1;
	} else if (suffix[1] && suffix[2]) {
		return -1;
	}

	switch (unit) {
	case 'k':
	case 'K':
		*scale = 1024ULL;
		return 0;
	case 'm':
	case 'M':
		*scale = 1024ULL * 1024ULL;
		return 0;
	case 'g':
	case 'G':
		*scale = 1024ULL * 1024ULL * 1024ULL;
		return 0;
	case 't':
	case 'T':
		*scale = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
		return 0;
	case 'p':
	case 'P':
		*scale = 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
		return 0;
	default:
		return -1;
	}
}

static uint64_t parse_u64_arg(const char *text, const char *name)
{
	char *end = NULL;
	uint64_t base;
	uint64_t scale;

	errno = 0;
	base = strtoull(text, &end, 0);
	if (errno || end == text || scale_for_suffix(end, &scale) ||
	    base > UINT64_MAX / scale) {
		fprintf(stderr, "Invalid %s: %s\n", name, text);
		exit(EXIT_FAILURE);
	}

	return base * scale;
}

static uint8_t parse_u8_arg(const char *text, const char *name)
{
	uint64_t value = parse_u64_arg(text, name);

	if (value > UINT8_MAX) {
		fprintf(stderr, "%s must be <= 0xff\n", name);
		exit(EXIT_FAILURE);
	}

	return (uint8_t)value;
}

static int add_overflows_u64(uint64_t a, uint64_t b, uint64_t *result)
{
	if (a > UINT64_MAX - b)
		return 1;
	*result = a + b;
	return 0;
}

static long get_page_size(void)
{
	long page_size = sysconf(_SC_PAGESIZE);

	return page_size > 0 ? page_size : (long)DEFAULT_PAGE_SIZE;
}

static uint64_t page_align_down(uint64_t value, uint64_t page_size)
{
	return (value / page_size) * page_size;
}

static uint64_t page_align_up(uint64_t value, uint64_t page_size)
{
	uint64_t rounded;

	if (add_overflows_u64(value, page_size - 1, &rounded) || rounded > SIZE_MAX) {
		fprintf(stderr, "mapping size is too large\n");
		exit(EXIT_FAILURE);
	}

	return (rounded / page_size) * page_size;
}

static void *map_physmem(int fd, uint64_t target_offset, uint64_t access_size,
			 uint64_t *map_offset_out, uint64_t *map_size_out,
			 uint64_t *offset_in_map_out)
{
	uint64_t page_size = (uint64_t)get_page_size();
	uint64_t map_offset;
	uint64_t offset_in_map;
	uint64_t map_span;
	uint64_t map_size;
	void *map_addr;

	if (!access_size) {
		fprintf(stderr, "access size must be > 0\n");
		exit(EXIT_FAILURE);
	}
	if (access_size > SIZE_MAX) {
		fprintf(stderr, "access size is too large\n");
		exit(EXIT_FAILURE);
	}
	if (add_overflows_u64(target_offset, access_size, &map_span)) {
		fprintf(stderr, "target offset plus access size overflows\n");
		exit(EXIT_FAILURE);
	}

	map_offset = page_align_down(target_offset, page_size);
	offset_in_map = target_offset - map_offset;
	if (add_overflows_u64(offset_in_map, access_size, &map_span)) {
		fprintf(stderr, "mmap span overflows\n");
		exit(EXIT_FAILURE);
	}
	map_size = page_align_up(map_span, page_size);

	map_addr = mmap(NULL, (size_t)map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			fd, (off_t)map_offset);
	if (map_addr == MAP_FAILED) {
		fprintf(stderr,
			"mmap failed: %s map_offset=0x%" PRIx64
			" map_size=0x%" PRIx64 " target_offset=0x%" PRIx64
			" access_size=0x%" PRIx64 "\n",
			strerror(errno), map_offset, map_size, target_offset, access_size);
		exit(EXIT_FAILURE);
	}

	*map_offset_out = map_offset;
	*map_size_out = map_size;
	*offset_in_map_out = offset_in_map;
	return map_addr;
}

static double monotonic_seconds(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
		fprintf(stderr, "clock_gettime failed: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void validate_gpu_access(void *map_addr, uint64_t offset_in_map,
				uint64_t size, uint8_t value)
{
	uint8_t *host = NULL;
	void *d_write = NULL;
	void *d_read = NULL;

	host = malloc((size_t)size);
	if (!host) {
		fprintf(stderr, "malloc failed: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	CHECK_MC(mcMalloc(&d_write, (size_t)size));
	CHECK_MC(mcMalloc(&d_read, (size_t)size));
	CHECK_MC(mcMemset(d_write, value, (size_t)size));
	CHECK_MC(mcMemcpy((uint8_t *)map_addr + offset_in_map, d_write, (size_t)size,
			mcMemcpyDeviceToHost));
	CHECK_MC(mcMemcpy(d_read, (uint8_t *)map_addr + offset_in_map, (size_t)size,
			mcMemcpyHostToDevice));
	CHECK_MC(mcMemcpy(host, d_read, (size_t)size, mcMemcpyDeviceToHost));
	CHECK_MC(mcDeviceSynchronize());

	for (uint64_t i = 0; i < size; i++) {
		if (host[i] != value) {
			fprintf(stderr,
				"validation failed at byte %" PRIu64 ": read 0x%02x, expected 0x%02x\n",
				i, host[i], value);
			exit(EXIT_FAILURE);
		}
	}

	CHECK_MC(mcFree(d_read));
	CHECK_MC(mcFree(d_write));
	free(host);
	printf("Validation passed: GPU write/read data matched 0x%02x over %" PRIu64
	       " bytes.\n", value, size);
}

static double run_read_bandwidth(void *map_addr, uint64_t offset_in_map,
				 uint64_t size, uint64_t iters)
{
	void *d_buf = NULL;
	double start;
	double elapsed;

	CHECK_MC(mcMalloc(&d_buf, (size_t)size));
	CHECK_MC(mcMemcpy(d_buf, (uint8_t *)map_addr + offset_in_map, (size_t)size,
			mcMemcpyHostToDevice));
	CHECK_MC(mcDeviceSynchronize());

	start = monotonic_seconds();
	for (uint64_t i = 0; i < iters; i++)
		CHECK_MC(mcMemcpy(d_buf, (uint8_t *)map_addr + offset_in_map, (size_t)size,
				mcMemcpyHostToDevice));
	CHECK_MC(mcDeviceSynchronize());
	elapsed = monotonic_seconds() - start;

	CHECK_MC(mcFree(d_buf));
	return elapsed;
}

static double run_write_bandwidth(void *map_addr, uint64_t offset_in_map,
				  uint64_t size, uint64_t iters, uint8_t value)
{
	void *d_buf = NULL;
	double start;
	double elapsed;

	CHECK_MC(mcMalloc(&d_buf, (size_t)size));
	CHECK_MC(mcMemset(d_buf, value, (size_t)size));
	CHECK_MC(mcDeviceSynchronize());

	start = monotonic_seconds();
	for (uint64_t i = 0; i < iters; i++)
		CHECK_MC(mcMemcpy((uint8_t *)map_addr + offset_in_map, d_buf, (size_t)size,
				mcMemcpyDeviceToHost));
	CHECK_MC(mcDeviceSynchronize());
	elapsed = monotonic_seconds() - start;

	CHECK_MC(mcFree(d_buf));
	return elapsed;
}

static void parse_options(int argc, char **argv, struct options *opt,
			  const char **dev_arg)
{
	opt->size = DEFAULT_SIZE_MB * 1024ULL * 1024ULL;
	opt->offset = 0;
	opt->iters = DEFAULT_ITERS;
	opt->write_value = DEFAULT_WRITE_VALUE;
	opt->gpu = 0;
	*dev_arg = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			uint64_t size_mb = parse_u64_arg(argv[++i], "size");

			if (size_mb > UINT64_MAX / (1024ULL * 1024ULL)) {
				fprintf(stderr, "size is too large\n");
				exit(EXIT_FAILURE);
			}
			opt->size = size_mb * 1024ULL * 1024ULL;
		} else if (!strcmp(argv[i], "--offset") && i + 1 < argc) {
			opt->offset = parse_u64_arg(argv[++i], "offset");
		} else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
			opt->iters = parse_u64_arg(argv[++i], "iters");
		} else if (!strcmp(argv[i], "--gpu") && i + 1 < argc) {
			uint64_t gpu = parse_u64_arg(argv[++i], "gpu");

			if (gpu > INT_MAX) {
				fprintf(stderr, "gpu is too large\n");
				exit(EXIT_FAILURE);
			}
			opt->gpu = (int)gpu;
		} else if ((!strcmp(argv[i], "--value") || !strcmp(argv[i], "--data")) &&
			   i + 1 < argc) {
			opt->write_value = parse_u8_arg(argv[++i], "value");
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "Unknown option: %s\n", argv[i]);
			usage(argv[0]);
			exit(EXIT_FAILURE);
		} else if (!*dev_arg) {
			*dev_arg = argv[i];
		} else {
			fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (!opt->size) {
		fprintf(stderr, "size must be > 0\n");
		exit(EXIT_FAILURE);
	}
	if (!opt->iters) {
		fprintf(stderr, "iters must be > 0\n");
		exit(EXIT_FAILURE);
	}
}

static double bandwidth_gib_per_second(uint64_t size, uint64_t iters,
					      double seconds)
{
	long double bytes = (long double)size * (long double)iters;

	if (seconds <= 0.0)
		return 0.0;
	return (double)(bytes / (1024.0L * 1024.0L * 1024.0L) / seconds);
}

int main(int argc, char **argv)
{
	struct options opt;
	const char *dev_arg;
	const char *dev_path;
	char dev_path_buf[PATH_MAX];
	uint64_t map_offset;
	uint64_t map_size;
	uint64_t offset_in_map;
	void *map_addr;
	double read_seconds;
	double write_seconds;
	int fd;

	parse_options(argc, argv, &opt, &dev_arg);
	dev_path = resolve_dev_path(dev_arg, dev_path_buf, sizeof(dev_path_buf));

	load_mc_runtime();
	CHECK_MC(mcSetDevice(opt.gpu));
	fd = open(dev_path, O_RDWR | O_SYNC | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", dev_path, strerror(errno));
		return EXIT_FAILURE;
	}

	map_addr = map_physmem(fd, opt.offset, opt.size, &map_offset, &map_size,
			       &offset_in_map);
	CHECK_MC(mcHostRegister(map_addr, (size_t)map_size, mcHostRegisterIoMemory));

	printf("GPU char-device bandwidth test:\n");
	printf("  device        = %s\n", dev_path);
	printf("  gpu           = %d\n", opt.gpu);
	printf("  target_offset = 0x%" PRIx64 "\n", opt.offset);
	printf("  size          = %" PRIu64 " MiB (%" PRIu64 " bytes)\n",
	       (uint64_t)(opt.size / (1024ULL * 1024ULL)), opt.size);
	printf("  iters         = %" PRIu64 "\n", opt.iters);
	printf("  write_value   = 0x%02x\n", opt.write_value);
	printf("  mmap_offset   = 0x%" PRIx64 "\n", map_offset);
	printf("  mmap_size     = 0x%" PRIx64 " (%" PRIu64 " bytes)\n\n",
	       map_size, map_size);

	validate_gpu_access(map_addr, offset_in_map, opt.size, opt.write_value);

	printf("Running GPU read bandwidth first...\n");
	read_seconds = run_read_bandwidth(map_addr, offset_in_map, opt.size,
					 opt.iters);
	printf("READ  bandwidth: %.3f GiB/s (%" PRIu64 " bytes x %" PRIu64
	       " iters in %.6f s)\n",
	       bandwidth_gib_per_second(opt.size, opt.iters, read_seconds),
	       opt.size, opt.iters, read_seconds);

	printf("Running GPU write bandwidth with data 0x%02x...\n", opt.write_value);
	write_seconds = run_write_bandwidth(map_addr, offset_in_map, opt.size,
					   opt.iters, opt.write_value);
	printf("WRITE bandwidth: %.3f GiB/s (%" PRIu64 " bytes x %" PRIu64
	       " iters in %.6f s)\n",
	       bandwidth_gib_per_second(opt.size, opt.iters, write_seconds),
	       opt.size, opt.iters, write_seconds);

	CHECK_MC(mcHostUnregister(map_addr));
	munmap(map_addr, (size_t)map_size);
	close(fd);
	return EXIT_SUCCESS;
}
