// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <mc_runtime.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "map_va_to_pa.h"
#include "physmap_ioctl.h"

#define DEFAULT_CTL_DEV "/dev/physmap_ctl"
#define DEFAULT_DEV_PATH "/dev/physmap0"
#define DEFAULT_SIZE_MB 1024ULL
#define DEFAULT_ITERS 100
#define DEFAULT_WRITE_VALUE 0xa5U

#define TEST_READ  (1U << 0)
#define TEST_WRITE (1U << 1)

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

struct options {
	const char *device_arg;
	const char *path;
	size_t size_mb;
	uint64_t offset_bytes;
	uint64_t phys_addr;
	uint64_t map_size_bytes;
	int iterations;
	int device;
	uint32_t gpu_id;
	uint8_t write_value;
	int is_cpu_mem;
	unsigned int test_mask;
	int continuous;
};

struct physmap_device_info {
	const char *path;
	uint64_t phys_addr;
	uint64_t size;
};

struct host_buffer {
	void *base;
	void *data;
	int fd;
};

static volatile sig_atomic_t stop_requested;

static void handle_stop_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void read_physmap_list(struct physmap_list_req *req, const char *what)
{
	int fd = open(DEFAULT_CTL_DEV, O_RDWR | O_CLOEXEC);

	if (fd < 0) {
		fprintf(stderr, "open %s failed while resolving %s: %s\n",
			DEFAULT_CTL_DEV, what, strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (ioctl(fd, PHYSMAP_IOC_LIST, req) < 0) {
		fprintf(stderr, "PHYSMAP_IOC_LIST failed while resolving %s: %s\n",
			what, strerror(errno));
		close(fd);
		exit(EXIT_FAILURE);
	}
	close(fd);
}

static void fill_device_info(const struct physmap_list_entry *entry,
			     char *path_buf, size_t path_buf_len,
			     struct physmap_device_info *info)
{
	int written = snprintf(path_buf, path_buf_len, "%s", entry->dev_name);

	if (written < 0 || (size_t)written >= path_buf_len) {
		fprintf(stderr, "device path is too long: %s\n", entry->dev_name);
		exit(EXIT_FAILURE);
	}

	info->path = path_buf;
	info->phys_addr = entry->phys_addr;
	info->size = entry->size;
}

static void resolve_device_info(const char *device, char *path_buf,
				 size_t path_buf_len,
				 struct physmap_device_info *info)
{
	struct physmap_list_req req = { 0 };
	const char *lookup = (!device || !*device) ? DEFAULT_DEV_PATH : device;
	uint32_t count;

	read_physmap_list(&req, lookup);
	count = req.count > PHYSMAP_MAX_MAPPINGS ? PHYSMAP_MAX_MAPPINGS : req.count;
	for (uint32_t i = 0; i < count; i++) {
		if (!strcmp(req.entries[i].identifier, lookup) ||
		    !strcmp(req.entries[i].dev_name, lookup)) {
			fill_device_info(&req.entries[i], path_buf, path_buf_len, info);
			return;
		}
	}

	fprintf(stderr, "physmap device not found in %s: %s\n", DEFAULT_CTL_DEV,
		lookup);
	exit(EXIT_FAILURE);
}

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

static void usage(const char *prog)
{
	printf("Usage:\n");
	printf("  %s [--size MB] [--offset BYTES] [--iters N] [--gpu ID]\n", prog);
	printf("     [--direction read|write|both] [--read-only] [--write-only]\n");
	printf("     [--continuous] [--is_cpu_mem] [--path DEVICE|device]\n\n");
	printf("Maps the full physmap character device, registers that complete VA range\n");
	printf("with map_va_to_pa(), and measures DMA bandwidth on the requested subrange.\n");
	printf("The physical address and map size are resolved from %s.\n\n",
	       DEFAULT_CTL_DEV);
	printf("Options:\n");
	printf("  --size MB             transfer size in MiB, default %llu\n",
	       (unsigned long long)DEFAULT_SIZE_MB);
	printf("  --offset BYTES        offset inside the physmap device, default 0\n");
	printf("                        accepts decimal, hex, or suffix K/M/G/T\n");
	printf("  --iters N             iterations per measurement, default %d\n", DEFAULT_ITERS);
	printf("  --gpu ID              GPU id, default 0\n");
	printf("  --path DEVICE         physmap device path or identifier, default %s\n",
	       DEFAULT_DEV_PATH);
	printf("  --is_cpu_mem          pass CPU/system-memory flag to VA/PA map and unmap (default on)\n");
	printf("  --direction MODE      test read, write, or both directions, default both\n");
	printf("  --read-only           only test GPU read / host-to-device bandwidth\n");
	printf("  --write-only          only test GPU write / device-to-host bandwidth\n");
	printf("  --continuous          print bandwidth repeatedly until Ctrl-C/SIGTERM\n\n");
	printf("Examples:\n");
	printf("  %s --gpu 0 /dev/physmap0 --size 1024 --iters 100\n", prog);
	printf("  %s --gpu 0 h0-0 --offset 2G --size 1024 --read-only\n", prog);
}

static void parse_args(int argc, char **argv, struct options *opt)
{
	opt->device_arg = NULL;
	opt->path = DEFAULT_DEV_PATH;
	opt->size_mb = DEFAULT_SIZE_MB;
	opt->offset_bytes = 0;
	opt->phys_addr = 0;
	opt->map_size_bytes = 0;
	opt->iterations = DEFAULT_ITERS;
	opt->device = 0;
	opt->gpu_id = 0;
	opt->write_value = DEFAULT_WRITE_VALUE;
	opt->is_cpu_mem = 1;
	opt->test_mask = TEST_READ | TEST_WRITE;
	opt->continuous = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--path") && i + 1 < argc) {
			opt->device_arg = argv[++i];
		} else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			opt->size_mb = (size_t)parse_size_value(argv[++i], "--size");
		} else if (!strcmp(argv[i], "--offset") && i + 1 < argc) {
			opt->offset_bytes = parse_size_value(argv[++i], "--offset");
		} else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
			opt->iterations = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--gpu") && i + 1 < argc) {
			opt->device = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--direction") && i + 1 < argc) {
			i++;
			if (!strcmp(argv[i], "read"))
				opt->test_mask = TEST_READ;
			else if (!strcmp(argv[i], "write"))
				opt->test_mask = TEST_WRITE;
			else if (!strcmp(argv[i], "both"))
				opt->test_mask = TEST_READ | TEST_WRITE;
			else {
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
		} else if (!strcmp(argv[i], "--read-only")) {
			opt->test_mask = TEST_READ;
		} else if (!strcmp(argv[i], "--write-only")) {
			opt->test_mask = TEST_WRITE;
		} else if (!strcmp(argv[i], "--continuous")) {
			opt->continuous = 1;
		} else if (!strcmp(argv[i], "--is_cpu_mem")) {
			opt->is_cpu_mem = 1;
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		} else if (argv[i][0] != '-' && opt->device_arg == NULL) {
			opt->device_arg = argv[i];
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
}

static double bytes_to_gb(uint64_t bytes)
{
	return (double)bytes / 1000.0 / 1000.0 / 1000.0;
}

static void *mmap_host_buffer(const struct options *opt, int *fd_out)
{
	void *addr;
	int fd;

	fd = open(opt->path, O_RDWR | O_SYNC | O_CLOEXEC);
	CHECK_SYS(fd < 0, "open physmap device");

	addr = mmap(NULL, (size_t)opt->map_size_bytes, PROT_READ | PROT_WRITE,
		    MAP_SHARED, fd, 0);
	CHECK_SYS(addr == MAP_FAILED, "mmap physmap device");

	*fd_out = fd;
	return addr;
}

static struct host_buffer alloc_host_buffer(const struct options *opt)
{
	struct host_buffer h = { 0 };

	h.fd = -1;
	h.base = mmap_host_buffer(opt, &h.fd);
	h.data = (uint8_t *)h.base + opt->offset_bytes;

	if (map_va_to_pa(opt->gpu_id, (uint64_t)(uintptr_t)h.base, opt->phys_addr,
			 opt->map_size_bytes, opt->is_cpu_mem))
		exit(EXIT_FAILURE);
	return h;
}

static void free_host_buffer(const struct options *opt, struct host_buffer *h)
{
	if (h->base == NULL)
		return;

	if (unmap_va_to_pa(opt->gpu_id, (uint64_t)(uintptr_t)h->base,
			 opt->map_size_bytes, opt->is_cpu_mem))
		exit(EXIT_FAILURE);
	munmap(h->base, (size_t)opt->map_size_bytes);
	if (h->fd >= 0)
		close(h->fd);
	h->base = NULL;
	h->data = NULL;
	h->fd = -1;
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

static double measure_read_bandwidth(void *h_buf, void *d_buf, size_t size,
				     int iterations, mcStream_t stream,
				     mcEvent_t start, mcEvent_t stop)
{
	float ms = 0.0f;
	uint64_t total_bytes = (uint64_t)size * (uint64_t)iterations;

	CHECK_MC(mcEventRecord(start, stream));
	for (int i = 0; i < iterations; i++)
		CHECK_MC(mcMemcpyAsync(d_buf, h_buf, size, mcMemcpyHostToDevice, stream));
	CHECK_MC(mcEventRecord(stop, stream));
	CHECK_MC(mcEventSynchronize(stop));
	CHECK_MC(mcEventElapsedTime(&ms, start, stop));

	return elapsed_bandwidth_gb(total_bytes, ms);
}

static double measure_write_bandwidth(void *h_buf, void *d_buf, size_t size,
				      int iterations, mcStream_t stream,
				      mcEvent_t start, mcEvent_t stop)
{
	float ms = 0.0f;
	uint64_t total_bytes = (uint64_t)size * (uint64_t)iterations;

	CHECK_MC(mcEventRecord(start, stream));
	for (int i = 0; i < iterations; i++)
		CHECK_MC(mcMemcpyAsync(h_buf, d_buf, size, mcMemcpyDeviceToHost, stream));
	CHECK_MC(mcEventRecord(stop, stream));
	CHECK_MC(mcEventSynchronize(stop));
	CHECK_MC(mcEventElapsedTime(&ms, start, stop));

	return elapsed_bandwidth_gb(total_bytes, ms);
}

static void print_test_config(const struct options *opt, size_t size,
			      uint64_t total_bytes)
{
	printf("\n=== MUSA DMA Bandwidth V2P Test ===\n");
	printf("device path       : %s\n", opt->path);
	printf("mmap offset       : 0x%llx bytes (%.2f MiB)\n",
	       (unsigned long long)opt->offset_bytes,
	       (double)opt->offset_bytes / 1024.0 / 1024.0);
	printf("register method   : map_va_to_pa KMD ioctl\n");
	printf("register pa       : 0x%llx\n",
	       (unsigned long long)opt->phys_addr);
	printf("register size     : 0x%llx bytes\n",
	       (unsigned long long)opt->map_size_bytes);
	printf("is cpu mem        : %s\n", opt->is_cpu_mem ? "yes" : "no");
	printf("buffer size       : %.2f MiB\n", (double)size / 1024.0 / 1024.0);
	printf("iterations        : %d%s\n", opt->iterations,
	       opt->continuous ? " per update" : "");
	printf("direction         : %s\n",
	       opt->test_mask == TEST_READ ? "read" :
	       opt->test_mask == TEST_WRITE ? "write" : "both");
	printf("continuous        : %s\n", opt->continuous ? "yes" : "no");
	printf("write value       : 0x%02x\n", opt->write_value);
	printf("total transfer    : %.2f GB%s\n\n", bytes_to_gb(total_bytes),
	       opt->continuous ? " per direction/update" : "");
}

static void run_test(const struct options *opt)
{
	size_t size = opt->size_mb * 1024ULL * 1024ULL;
	struct host_buffer h_buf;
	void *d_buf = NULL;
	mcStream_t stream;
	mcEvent_t start;
	mcEvent_t stop;
	uint64_t total_bytes = (uint64_t)size * (uint64_t)opt->iterations;

	h_buf = alloc_host_buffer(opt);
	CHECK_MC(mcMalloc(&d_buf, size));
	CHECK_MC(mcStreamCreate(&stream));
	CHECK_MC(mcEventCreate(&start));
	CHECK_MC(mcEventCreate(&stop));

	validate_read_write(h_buf.data, d_buf, size, opt->write_value, stream);

	for (int i = 0; i < 5; i++)
		CHECK_MC(mcMemcpyAsync(d_buf, h_buf.data, size, mcMemcpyHostToDevice, stream));
	CHECK_MC(mcMemset(d_buf, opt->write_value, size));
	CHECK_MC(mcStreamSynchronize(stream));

	print_test_config(opt, size, total_bytes);

	if (opt->continuous) {
		unsigned long update = 0;

		printf("Press Ctrl-C to stop.\n");
		while (!stop_requested) {
			update++;
			printf("[%lu]", update);
			if (opt->test_mask & TEST_READ)
				printf(" READ %.2f GB/s",
				       measure_read_bandwidth(h_buf.data, d_buf, size,
							      opt->iterations, stream,
							      start, stop));
			if (opt->test_mask & TEST_WRITE)
				printf(" WRITE %.2f GB/s",
				       measure_write_bandwidth(h_buf.data, d_buf, size,
						       opt->iterations, stream,
						       start, stop));
			printf("\n");
			fflush(stdout);
		}
		printf("Stopping continuous bandwidth test.\n");
	} else {
		if (opt->test_mask & TEST_READ)
			printf("READ  bandwidth   : %.2f GB/s\n",
			       measure_read_bandwidth(h_buf.data, d_buf, size,
						      opt->iterations, stream, start, stop));
		if (opt->test_mask & TEST_WRITE)
			printf("WRITE bandwidth   : %.2f GB/s\n",
			       measure_write_bandwidth(h_buf.data, d_buf, size,
						       opt->iterations, stream, start, stop));
	}

	CHECK_MC(mcEventDestroy(start));
	CHECK_MC(mcEventDestroy(stop));
	CHECK_MC(mcStreamDestroy(stream));
	CHECK_MC(mcFree(d_buf));
	free_host_buffer(opt, &h_buf);
}

int main(int argc, char **argv)
{
	struct options opt;
	mcDeviceProp_t prop;
	struct physmap_device_info dev_info;
	char dev_path_buf[PATH_MAX];
	uint64_t size;

	parse_args(argc, argv, &opt);
	resolve_device_info(opt.device_arg, dev_path_buf, sizeof(dev_path_buf),
			    &dev_info);
	opt.path = dev_info.path;
	opt.phys_addr = dev_info.phys_addr;
	opt.map_size_bytes = dev_info.size;
	size = (uint64_t)opt.size_mb * 1024ULL * 1024ULL;
	if (size > opt.map_size_bytes || opt.offset_bytes > opt.map_size_bytes - size) {
		fprintf(stderr, "ERROR: requested range exceeds physmap device\n");
		return EXIT_FAILURE;
	}

	CHECK_MC(mcSetDevice(opt.device));
	opt.gpu_id = gpu_index_to_id((uint32_t)opt.device);
	CHECK_MC(mcGetDeviceProperties(&prop, opt.device));
	signal(SIGINT, handle_stop_signal);
	signal(SIGTERM, handle_stop_signal);

	printf("MUSA device       : %d\n", opt.device);
	printf("MUSA GPU name     : %s\n", prop.name);

	run_test(&opt);
	return 0;
}
