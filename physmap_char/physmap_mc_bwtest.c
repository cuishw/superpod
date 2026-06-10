// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <mc_runtime.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_SIZE_MB 1024ULL
#define DEFAULT_ITERS 100
#define DEFAULT_WRITE_VALUE 0xa5U

#define CHECK_MC(call)                                                       \
	do {                                                                     \
		mcError_t err = (call);                                             \
		if (err != mcSuccess) {                                            \
			fprintf(stderr, "MC error %s:%d: %s\n", __FILE__, __LINE__,  \
				mcGetErrorString(err));                                  \
			exit(EXIT_FAILURE);                                        \
		}                                                                  \
	} while (0)

#define CHECK_SYS(cond, msg)                                                 \
	do {                                                                     \
		if (cond) {                                                        \
			fprintf(stderr, "%s failed: %s\n", msg, strerror(errno));    \
			exit(EXIT_FAILURE);                                        \
		}                                                                  \
	} while (0)

enum map_mode {
	MODE_ANON,
	MODE_FILE,
};

enum host_mode {
	HOST_MMAP_REGISTER,
	HOST_MALLOC_HOST,
};

struct options {
	enum map_mode mode;
	enum host_mode host;
	const char *path;
	size_t size_mb;
	uint64_t offset_bytes;
	int iterations;
	int device;
	int use_io_memory;
	uint8_t write_value;
};

static uint64_t parse_size_value(const char *s, const char *name)
{
	char *end = NULL;
	uint64_t value;

	errno = 0;
	value = strtoull(s, &end, 0);
	if (errno != 0 || end == s) {
		fprintf(stderr, "ERROR: invalid %s: %s\n", name, s);
		exit(EXIT_FAILURE);
	}

	while (*end != '\0' && isspace((unsigned char)*end))
		end++;

	if (*end != '\0') {
		char suffix = (char)tolower((unsigned char)*end++);

		if (*end == 'i' || *end == 'I')
			end++;
		if (*end == 'b' || *end == 'B')
			end++;

		while (*end != '\0' && isspace((unsigned char)*end))
			end++;

		if (*end != '\0') {
			fprintf(stderr, "ERROR: invalid %s suffix: %s\n", name, s);
			exit(EXIT_FAILURE);
		}

		switch (suffix) {
		case 'k':
			value *= 1024ULL;
			break;
		case 'm':
			value *= 1024ULL * 1024ULL;
			break;
		case 'g':
			value *= 1024ULL * 1024ULL * 1024ULL;
			break;
		case 't':
			value *= 1024ULL * 1024ULL * 1024ULL * 1024ULL;
			break;
		default:
			fprintf(stderr, "ERROR: invalid %s suffix: %s\n", name, s);
			exit(EXIT_FAILURE);
		}
	}

	return value;
}

static uint8_t parse_u8_value(const char *s, const char *name)
{
	uint64_t value = parse_size_value(s, name);

	if (value > UINT8_MAX) {
		fprintf(stderr, "ERROR: %s must be <= 0xff\n", name);
		exit(EXIT_FAILURE);
	}

	return (uint8_t)value;
}

static void usage(const char *prog)
{
	printf("Usage:\n");
	printf("  %s [--host mmap-register|malloc-host] [--mode anon|file] [--path PATH]\n",
	       prog);
	printf("     [--size MB] [--offset BYTES] [--iters N] [--gpu ID] [--io]\n");
	printf("     [--value BYTE|--data BYTE]\n");
	printf("\n");

	printf("Host memory modes:\n");
	printf("  --host mmap-register   mmap + mcHostRegister, default\n");
	printf("  --host malloc-host     mcMallocHost pinned memory\n");
	printf("\n");

	printf("Options:\n");
	printf("  --size MB             buffer size in MiB, default %llu\n",
	       (unsigned long long)DEFAULT_SIZE_MB);
	printf("  --offset BYTES        mmap offset inside file/BAR resource, default 0\n");
	printf("                        accepts decimal, hex, or suffix K/M/G/T, e.g. 0x200000000 or 2G\n");
	printf("                        file/BAR mmap offset must be page aligned\n");
	printf("  --iters N             iterations, default %d\n", DEFAULT_ITERS);
	printf("  --gpu ID              GPU id, default 0\n");
	printf("  --io                  use mcHostRegisterIoMemory for mmap-register\n");
	printf("  --value BYTE          byte value used for validation and write bandwidth, default 0x%02x\n",
	       DEFAULT_WRITE_VALUE);
	printf("\n");

	printf("Examples:\n");
	printf("  %s --host mmap-register --mode anon --size 1024 --iters 100\n",
	       prog);
	printf("  %s --host malloc-host --size 1024 --iters 100\n", prog);
	printf("  %s --host mmap-register --mode file --path /sys/bus/pci/devices/0000:86:00.0/resource2 --size 1024 --offset 2G --iters 100 --io\n",
	       prog);
}

static void parse_args(int argc, char **argv, struct options *opt)
{
	opt->mode = MODE_ANON;
	opt->host = HOST_MMAP_REGISTER;
	opt->path = NULL;
	opt->size_mb = DEFAULT_SIZE_MB;
	opt->offset_bytes = 0;
	opt->iterations = DEFAULT_ITERS;
	opt->device = 0;
	opt->use_io_memory = 0;
	opt->write_value = DEFAULT_WRITE_VALUE;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--host") && i + 1 < argc) {
			i++;
			if (!strcmp(argv[i], "mmap-register"))
				opt->host = HOST_MMAP_REGISTER;
			else if (!strcmp(argv[i], "malloc-host"))
				opt->host = HOST_MALLOC_HOST;
			else {
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
		} else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
			i++;
			if (!strcmp(argv[i], "anon"))
				opt->mode = MODE_ANON;
			else if (!strcmp(argv[i], "file"))
				opt->mode = MODE_FILE;
			else {
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
		} else if (!strcmp(argv[i], "--path") && i + 1 < argc) {
			opt->path = argv[++i];
		} else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			opt->size_mb = (size_t)parse_size_value(argv[++i], "--size");
		} else if (!strcmp(argv[i], "--offset") && i + 1 < argc) {
			opt->offset_bytes = parse_size_value(argv[++i], "--offset");
		} else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
			opt->iterations = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--gpu") && i + 1 < argc) {
			opt->device = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--io")) {
			opt->use_io_memory = 1;
		} else if ((!strcmp(argv[i], "--value") || !strcmp(argv[i], "--data")) &&
			   i + 1 < argc) {
			opt->write_value = parse_u8_value(argv[++i], "--value");
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		} else {
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (opt->size_mb == 0) {
		fprintf(stderr, "ERROR: --size must be > 0\n");
		exit(EXIT_FAILURE);
	}
	if (opt->iterations <= 0) {
		fprintf(stderr, "ERROR: --iters must be > 0\n");
		exit(EXIT_FAILURE);
	}

	if (opt->host == HOST_MALLOC_HOST) {
		if (opt->path != NULL || opt->use_io_memory || opt->offset_bytes != 0) {
			fprintf(stderr,
				"ERROR: --host malloc-host does not use --path, --offset or --io\n");
			exit(EXIT_FAILURE);
		}
	}

	if (opt->host == HOST_MMAP_REGISTER && opt->mode == MODE_ANON &&
	    opt->offset_bytes != 0) {
		fprintf(stderr, "ERROR: --offset is only valid with --mode file\n");
		exit(EXIT_FAILURE);
	}

	if (opt->host == HOST_MMAP_REGISTER && opt->mode == MODE_FILE &&
	    opt->path == NULL) {
		fprintf(stderr, "ERROR: --mode file requires --path\n");
		exit(EXIT_FAILURE);
	}

	if (opt->host == HOST_MMAP_REGISTER && opt->mode == MODE_FILE) {
		long page_size = sysconf(_SC_PAGE_SIZE);

		if (page_size <= 0)
			page_size = 4096;

		if ((opt->offset_bytes % (uint64_t)page_size) != 0) {
			fprintf(stderr,
				"ERROR: --offset must be page aligned, page size is %ld bytes\n",
				page_size);
			exit(EXIT_FAILURE);
		}
	}
}

static const char *host_mode_name(enum host_mode host)
{
	switch (host) {
	case HOST_MMAP_REGISTER:
		return "mmap + mcHostRegister";
	case HOST_MALLOC_HOST:
		return "mcMallocHost";
	default:
		return "unknown";
	}
}

static const char *map_mode_name(enum map_mode mode)
{
	switch (mode) {
	case MODE_ANON:
		return "anonymous mmap";
	case MODE_FILE:
		return "file/BAR mmap";
	default:
		return "unknown";
	}
}

static double bytes_to_gb(uint64_t bytes)
{
	return (double)bytes / 1000.0 / 1000.0 / 1000.0;
}

static void *mmap_host_buffer(const struct options *opt, size_t size, int *fd_out)
{
	void *addr = NULL;
	int fd = -1;

	if (opt->mode == MODE_ANON) {
		addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		CHECK_SYS(addr == MAP_FAILED, "mmap anonymous");
	} else {
		fd = open(opt->path, O_RDWR | O_SYNC);
		CHECK_SYS(fd < 0, "open");

		if (opt->offset_bytes > (uint64_t)LLONG_MAX) {
			fprintf(stderr, "ERROR: --offset is too large for off_t: 0x%llx\n",
				(unsigned long long)opt->offset_bytes);
			exit(EXIT_FAILURE);
		}

		addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			    (off_t)opt->offset_bytes);
		CHECK_SYS(addr == MAP_FAILED, "mmap file/BAR");
	}

	*fd_out = fd;
	return addr;
}

static void touch_buffer(uint8_t *buf, size_t size, uint8_t value)
{
	const size_t page = 4096;

	for (size_t off = 0; off < size; off += page)
		buf[off] = (uint8_t)(value + off);

	if (size > 0)
		buf[size - 1] = value;
}

static void *alloc_host_buffer(const struct options *opt, size_t size, int *fd_out)
{
	void *h_buf = NULL;
	*fd_out = -1;

	if (opt->host == HOST_MALLOC_HOST) {
		CHECK_MC(mcMallocHost(&h_buf, size));
		touch_buffer((uint8_t *)h_buf, size, opt->write_value);
		return h_buf;
	}

	h_buf = mmap_host_buffer(opt, size, fd_out);
	if (opt->mode == MODE_ANON)
		touch_buffer((uint8_t *)h_buf, size, opt->write_value);

	unsigned int reg_flags = mcHostRegisterDefault;

	if (opt->use_io_memory)
		reg_flags = mcHostRegisterIoMemory;

	CHECK_MC(mcHostRegister(h_buf, size, reg_flags));
	return h_buf;
}

static void free_host_buffer(const struct options *opt, void *h_buf, size_t size,
			     int fd)
{
	if (h_buf == NULL)
		return;

	if (opt->host == HOST_MALLOC_HOST) {
		CHECK_MC(mcFreeHost(h_buf));
		return;
	}

	CHECK_MC(mcHostUnregister(h_buf));
	munmap(h_buf, size);
	if (fd >= 0)
		close(fd);
}

static void validate_read_write(void *h_buf, void *d_buf, size_t size,
				uint8_t value, mcStream_t stream)
{
	uint8_t *check = malloc(size);

	if (!check) {
		fprintf(stderr, "malloc validation buffer failed: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	CHECK_MC(mcMemset(d_buf, value, size));
	CHECK_MC(mcMemcpyAsync(h_buf, d_buf, size, mcMemcpyDeviceToHost, stream));
	CHECK_MC(mcMemcpyAsync(d_buf, h_buf, size, mcMemcpyHostToDevice, stream));
	CHECK_MC(mcMemcpyAsync(check, d_buf, size, mcMemcpyDeviceToHost, stream));
	CHECK_MC(mcStreamSynchronize(stream));

	for (size_t i = 0; i < size; i++) {
		if (check[i] != value) {
			fprintf(stderr,
				"ERROR: validation failed at byte %zu: read 0x%02x expected 0x%02x\n",
				i, check[i], value);
			exit(EXIT_FAILURE);
		}
	}

	free(check);
	printf("validation        : write/read matched 0x%02x over %.2f MiB\n",
	       value, (double)size / 1024.0 / 1024.0);
}

static double elapsed_bandwidth_gb(uint64_t bytes, float ms)
{
	if (ms <= 0.0f)
		return 0.0;
	return bytes_to_gb(bytes) / ((double)ms / 1000.0);
}

static void run_test(const struct options *opt)
{
	size_t size = opt->size_mb * 1024ULL * 1024ULL;
	int fd = -1;
	void *h_buf;
	void *d_buf = NULL;
	mcStream_t stream;
	mcEvent_t start;
	mcEvent_t stop;
	float read_ms = 0.0f;
	float write_ms = 0.0f;
	uint64_t total_bytes;

	h_buf = alloc_host_buffer(opt, size, &fd);
	CHECK_MC(mcMalloc(&d_buf, size));
	CHECK_MC(mcStreamCreate(&stream));
	CHECK_MC(mcEventCreate(&start));
	CHECK_MC(mcEventCreate(&stop));

	validate_read_write(h_buf, d_buf, size, opt->write_value, stream);

	for (int i = 0; i < 5; i++)
		CHECK_MC(mcMemcpyAsync(d_buf, h_buf, size, mcMemcpyHostToDevice, stream));
	CHECK_MC(mcStreamSynchronize(stream));

	CHECK_MC(mcEventRecord(start, stream));
	for (int i = 0; i < opt->iterations; i++)
		CHECK_MC(mcMemcpyAsync(d_buf, h_buf, size, mcMemcpyHostToDevice, stream));
	CHECK_MC(mcEventRecord(stop, stream));
	CHECK_MC(mcEventSynchronize(stop));
	CHECK_MC(mcEventElapsedTime(&read_ms, start, stop));

	CHECK_MC(mcMemset(d_buf, opt->write_value, size));
	CHECK_MC(mcStreamSynchronize(stream));
	CHECK_MC(mcEventRecord(start, stream));
	for (int i = 0; i < opt->iterations; i++)
		CHECK_MC(mcMemcpyAsync(h_buf, d_buf, size, mcMemcpyDeviceToHost, stream));
	CHECK_MC(mcEventRecord(stop, stream));
	CHECK_MC(mcEventSynchronize(stop));
	CHECK_MC(mcEventElapsedTime(&write_ms, start, stop));

	total_bytes = (uint64_t)size * (uint64_t)opt->iterations;

	printf("\n=== MUSA DMA Bandwidth Test ===\n");
	printf("host mode         : %s\n", host_mode_name(opt->host));
	if (opt->host == HOST_MMAP_REGISTER) {
		printf("mmap mode         : %s\n", map_mode_name(opt->mode));
		if (opt->mode == MODE_FILE) {
			printf("path              : %s\n", opt->path);
			printf("mmap offset       : 0x%llx bytes (%.2f MiB)\n",
			       (unsigned long long)opt->offset_bytes,
			       (double)opt->offset_bytes / 1024.0 / 1024.0);
		}
		printf("register flag     : %s\n",
		       opt->use_io_memory ? "mcHostRegisterIoMemory" :
					    "mcHostRegisterDefault");
	}
	printf("buffer size       : %.2f MiB\n", (double)size / 1024.0 / 1024.0);
	printf("iterations        : %d\n", opt->iterations);
	printf("write value       : 0x%02x\n", opt->write_value);
	printf("total transfer    : %.2f GB\n\n", bytes_to_gb(total_bytes));
	printf("READ  bandwidth   : %.2f GB/s\n",
	       elapsed_bandwidth_gb(total_bytes, read_ms));
	printf("WRITE bandwidth   : %.2f GB/s\n",
	       elapsed_bandwidth_gb(total_bytes, write_ms));

	CHECK_MC(mcEventDestroy(start));
	CHECK_MC(mcEventDestroy(stop));
	CHECK_MC(mcStreamDestroy(stream));
	CHECK_MC(mcFree(d_buf));
	free_host_buffer(opt, h_buf, size, fd);
}

int main(int argc, char **argv)
{
	struct options opt;
	mcDeviceProp_t prop;

	parse_args(argc, argv, &opt);
	CHECK_MC(mcSetDevice(opt.device));
	CHECK_MC(mcGetDeviceProperties(&prop, opt.device));

	printf("MUSA device       : %d\n", opt.device);
	printf("MUSA GPU name     : %s\n", prop.name);

	run_test(&opt);
	return 0;
}
