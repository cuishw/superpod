// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <mc_runtime.h>

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
#include <unistd.h>

#include "physmap_ioctl.h"
#include "map_va_to_pa.h"

#define DEFAULT_CTL_DEV "/dev/physmap_ctl"
#define DEFAULT_DEV_PATH "/dev/physmap0"
#define DEFAULT_PAGE_SIZE 4096UL
#define DEFAULT_MXCD_DEV "/dev/mxcd"
#define MXCD_GPU_NODE_START 2

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
	int gpu;
	uint32_t gpu_id;
	int system_mem;
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s [--gpu ID] [--system] [device] read  <offset> <size>\n"
		"  %s [--gpu ID] [--system] [device] write <offset> <size> <byte_value>\n"
		"\n"
		"If device is omitted, %s is used. Device may be a full /dev/... path\n"
		"or a physmap IDENTIFIER resolved through %s. Memory is registered by\n"
		"calling MXCD VA/PA map ioctls on %s for the full physmap character-device\n"
		"address space. Use --system for CPU memory.\n",
		prog, prog, DEFAULT_DEV_PATH, DEFAULT_CTL_DEV, DEFAULT_MXCD_DEV);
}

static int is_command(const char *arg)
{
	return !strcmp(arg, "read") || !strcmp(arg, "write");
}

struct physmap_device_info {
	const char *path;
	uint64_t phys_addr;
	uint64_t size;
};

static void read_physmap_list(struct physmap_list_req *req, const char *what)
{
	int fd;

	fd = open(DEFAULT_CTL_DEV, O_RDWR | O_CLOEXEC);
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

	return page_size > 0 ? page_size : DEFAULT_PAGE_SIZE;
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

static void dump_hex(const uint8_t *buf, uint64_t base_offset, uint64_t size)
{
	for (uint64_t i = 0; i < size; i++) {
		if (i % 16 == 0)
			printf("0x%016" PRIx64 ": ", base_offset + i);
		printf("%02x ", buf[i]);
		if (i % 16 == 15 || i + 1 == size)
			printf("\n");
	}
}

static void *map_physmem(int fd, uint64_t device_size, uint64_t target_offset,
			 uint64_t access_size, uint64_t *offset_in_map_out)
{
	uint64_t page_size = (uint64_t)get_page_size();
	void *map_addr;

	if (!access_size) {
		fprintf(stderr, "access size must be > 0\n");
		exit(EXIT_FAILURE);
	}
	if (!device_size) {
		fprintf(stderr, "physmap device size must be > 0\n");
		exit(EXIT_FAILURE);
	}
	if (target_offset > UINT64_MAX - access_size ||
	    target_offset > device_size || access_size > device_size - target_offset) {
		fprintf(stderr,
			"requested range exceeds physmap device: offset=0x%" PRIx64
			" size=0x%" PRIx64 " device_size=0x%" PRIx64 "\n",
			target_offset, access_size, device_size);
		exit(EXIT_FAILURE);
	}
	if (device_size != page_align_up(device_size, page_size)) {
		fprintf(stderr, "physmap device size must be page aligned: 0x%" PRIx64 "\n",
			device_size);
		exit(EXIT_FAILURE);
	}
	if (device_size > SIZE_MAX) {
		fprintf(stderr, "physmap device size is too large to mmap: 0x%" PRIx64 "\n",
			device_size);
		exit(EXIT_FAILURE);
	}

	map_addr = mmap(NULL, (size_t)device_size, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
	if (map_addr == MAP_FAILED) {
		fprintf(stderr,
			"mmap failed: %s map_offset=0x0 map_size=0x%" PRIx64
			" target_offset=0x%" PRIx64 " access_size=0x%" PRIx64 "\n",
			strerror(errno), device_size, target_offset, access_size);
		exit(EXIT_FAILURE);
	}

	*offset_in_map_out = target_offset;
	return map_addr;
}

static uint32_t gpu_index_to_id(uint32_t gpu_index)
{
	char path[160];
	FILE *fp;
	uint32_t gpu_id;
	int ret;

	ret = snprintf(path, sizeof(path),
		       "/sys/devices/virtual/mxcd/mxcd/layout/nodes/%u/gpu_id",
		       gpu_index + MXCD_GPU_NODE_START);
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		fprintf(stderr, "gpu_id sysfs path is too long\n");
		exit(EXIT_FAILURE);
	}

	fp = fopen(path, "r");
	if (!fp) {
		fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (fscanf(fp, "%u", &gpu_id) != 1) {
		fprintf(stderr, "read gpu_id from %s failed\n", path);
		fclose(fp);
		exit(EXIT_FAILURE);
	}

	fclose(fp);
	printf("gpu index %u -> node %u -> gpu_id %u\n",
	       gpu_index, gpu_index + MXCD_GPU_NODE_START, gpu_id);

	return gpu_id;
}

static void wait_for_unregister(void)
{
	int ch;

	printf("Press Enter to unregister mapping...");
	fflush(stdout);
	while ((ch = getchar()) != '\n' && ch != EOF)
		;
}

static void do_gpu_read(const struct options *opt, int fd,
			uint64_t phys_addr, uint64_t device_size,
			uint64_t offset, uint64_t size)
{
	uint64_t offset_in_map;
	void *map_addr;
	void *d_buf = NULL;
	uint8_t *h_dump;

	map_addr = map_physmem(fd, device_size, offset, size, &offset_in_map);
	if (map_va_to_pa(opt->gpu_id, (uint64_t)(uintptr_t)map_addr, phys_addr,
			 device_size, opt->system_mem))
		exit(EXIT_FAILURE);
	h_dump = malloc((size_t)size);
	if (!h_dump) {
		fprintf(stderr, "malloc failed: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	CHECK_MC(mcMalloc(&d_buf, (size_t)size));
	CHECK_MC(mcMemcpy(d_buf, (uint8_t *)map_addr + offset_in_map, (size_t)size,
			  mcMemcpyHostToDevice));
	CHECK_MC(mcMemcpy(h_dump, d_buf, (size_t)size, mcMemcpyDeviceToHost));
	CHECK_MC(mcDeviceSynchronize());

	printf("GPU READ:\n");
	printf("  target_offset = 0x%" PRIx64 "\n", offset);
	printf("  size          = 0x%" PRIx64 " (%" PRIu64 " bytes)\n", size, size);
	printf("  mmap_offset   = 0x0\n");
	printf("  mmap_size     = 0x%" PRIx64 " (%" PRIu64 " bytes)\n",
	       device_size, device_size);
	printf("  register_pa   = 0x%" PRIx64 "\n\n", phys_addr);
	dump_hex(h_dump, offset, size);

	CHECK_MC(mcFree(d_buf));
	free(h_dump);
	wait_for_unregister();
	if (unmap_va_to_pa(opt->gpu_id, (uint64_t)(uintptr_t)map_addr,
				   device_size, opt->system_mem))
		exit(EXIT_FAILURE);
	munmap(map_addr, (size_t)device_size);
}

static void do_gpu_write(const struct options *opt, int fd,
			 uint64_t phys_addr, uint64_t device_size,
			 uint64_t offset, uint64_t size, uint8_t value)
{
	uint64_t offset_in_map;
	void *map_addr;
	void *d_buf = NULL;

	map_addr = map_physmem(fd, device_size, offset, size, &offset_in_map);
	if (map_va_to_pa(opt->gpu_id, (uint64_t)(uintptr_t)map_addr, phys_addr,
			 device_size, opt->system_mem))
		exit(EXIT_FAILURE);

	CHECK_MC(mcMalloc(&d_buf, (size_t)size));
	CHECK_MC(mcMemset(d_buf, value, (size_t)size));
	CHECK_MC(mcMemcpy((uint8_t *)map_addr + offset_in_map, d_buf, (size_t)size,
			  mcMemcpyDeviceToHost));
	CHECK_MC(mcDeviceSynchronize());

	printf("GPU WRITE:\n");
	printf("  target_offset = 0x%" PRIx64 "\n", offset);
	printf("  size          = 0x%" PRIx64 " (%" PRIu64 " bytes)\n", size, size);
	printf("  value         = 0x%02x\n", value);
	printf("  mmap_offset   = 0x0\n");
	printf("  mmap_size     = 0x%" PRIx64 " (%" PRIu64 " bytes)\n",
	       device_size, device_size);
	printf("  register_pa   = 0x%" PRIx64 "\n\n", phys_addr);
	printf("gpu write done\n");

	CHECK_MC(mcFree(d_buf));
	wait_for_unregister();
	if (unmap_va_to_pa(opt->gpu_id, (uint64_t)(uintptr_t)map_addr,
				   device_size, opt->system_mem))
		exit(EXIT_FAILURE);
	munmap(map_addr, (size_t)device_size);
}

static void parse_options(int argc, char **argv, struct options *opt, int *argi)
{
	opt->gpu = 0;
	opt->gpu_id = 0;
	opt->system_mem = 0;
	*argi = 1;

	while (*argi < argc) {
		if (!strcmp(argv[*argi], "--gpu") && *argi + 1 < argc) {
			opt->gpu = (int)parse_u64_arg(argv[*argi + 1], "gpu");
			*argi += 2;
		} else if (!strcmp(argv[*argi], "--system")) {
			opt->system_mem = 1;
			*argi += 1;
		} else if (!strcmp(argv[*argi], "--help") || !strcmp(argv[*argi], "-h")) {
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		} else {
			break;
		}
	}
}

int main(int argc, char **argv)
{
	struct options opt;
	const char *dev_arg = NULL;
	struct physmap_device_info dev_info;
	char dev_path_buf[PATH_MAX];
	const char *cmd;
	int argi;
	int fd;

	if (argc < 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	parse_options(argc, argv, &opt, &argi);
	if (argi < argc && !is_command(argv[argi]))
		dev_arg = argv[argi++];
	if (argi >= argc) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	cmd = argv[argi++];
	if (!is_command(cmd)) {
		fprintf(stderr, "Unknown command: %s\n", cmd);
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	resolve_device_info(dev_arg, dev_path_buf, sizeof(dev_path_buf), &dev_info);

	CHECK_MC(mcSetDevice(opt.gpu));
	opt.gpu_id = gpu_index_to_id((uint32_t)opt.gpu);
	fd = open(dev_info.path, O_RDWR | O_SYNC | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", dev_info.path, strerror(errno));
		return EXIT_FAILURE;
	}

	if (!strcmp(cmd, "read")) {
		if (argc - argi != 2) {
			usage(argv[0]);
			close(fd);
			return EXIT_FAILURE;
		}
		do_gpu_read(&opt, fd, dev_info.phys_addr, dev_info.size,
			    parse_u64_arg(argv[argi], "offset"),
			    parse_u64_arg(argv[argi + 1], "size"));
	} else if (!strcmp(cmd, "write")) {
		if (argc - argi != 3) {
			usage(argv[0]);
			close(fd);
			return EXIT_FAILURE;
		}
		do_gpu_write(&opt, fd, dev_info.phys_addr, dev_info.size,
			     parse_u64_arg(argv[argi], "offset"),
			     parse_u64_arg(argv[argi + 1], "size"),
			     parse_u8_arg(argv[argi + 2], "byte_value"));
	} else {
		fprintf(stderr, "Unknown command: %s\n", cmd);
		usage(argv[0]);
		close(fd);
		return EXIT_FAILURE;
	}

	close(fd);
	return EXIT_SUCCESS;
}
