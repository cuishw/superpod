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

#define DEFAULT_CTL_DEV "/dev/physmap_ctl"
#define DEFAULT_DEV_PATH "/dev/physmap0"
#define DEFAULT_PAGE_SIZE 4096UL

#define CHECK_MC(call)                                                       \
	do {                                                                     \
		mcError_t err__ = (call);                                           \
		if (err__ != mcSuccess) {                                          \
			fprintf(stderr, "MC error %s:%d: %s\n", __FILE__, __LINE__,  \
				mcGetErrorString(err__));                                \
			exit(EXIT_FAILURE);                                        \
		}                                                                  \
	} while (0)

#define DEFAULT_MXCD_DEV "/dev/mxcd"
#define MXCD_GPU_NODE_START 2
#define MXCD_IOCTL_BASE 'K'
#define MXCD_IOWR(nr, type) _IOWR(MXCD_IOCTL_BASE, nr, type)
#define MXCD_IOC_VA_PA_MAP_FLAGS_SYSTEM (1U << 0)

struct mxcd_ioctl_va_to_pa_args {
	uint64_t va_addr;
	uint64_t pa_addr;
	uint32_t gpu_id;
	uint32_t pte_flags;
};

struct mxcd_ioctl_va_pa_map_args {
	uint64_t va_addr;
	uint64_t pa_addr;
	uint64_t size;
	uint32_t gpu_id;
	uint32_t map_flags;
};

struct mxcd_ioctl_va_pa_unmap_args {
	uint64_t va_addr;
	uint64_t size;
	uint32_t gpu_id;
	uint32_t map_flags;
};

#define MXCD_IOC_VA_TO_PA \
	MXCD_IOWR(0x11, struct mxcd_ioctl_va_to_pa_args)
#define MXCD_IOC_VA_PA_MAP \
	MXCD_IOWR(0x4d, struct mxcd_ioctl_va_pa_map_args)
#define MXCD_IOC_VA_PA_UNMAP \
	MXCD_IOWR(0x4e, struct mxcd_ioctl_va_pa_unmap_args)

struct options {
	int gpu;
	uint32_t gpu_id;
	const char *ioctl_dev;
	int system_mem;
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s [--gpu ID] [--ioctl-dev DEV] [--system] [device] read    <offset> <size>\n"
		"  %s [--gpu ID] [--ioctl-dev DEV] [--system] [device] write   <offset> <size> <byte_value>\n"
		"  %s [--gpu ID] [--ioctl-dev DEV] [--system] [device] read64  <offset>\n"
		"  %s [--gpu ID] [--ioctl-dev DEV] [--system] [device] write64 <offset> <u64_value>\n"
		"\n"
		"If device is omitted, %s is used. Device may be a full /dev/... path\n"
		"or a physmap IDENTIFIER resolved through %s. Memory is registered by\n"
		"calling MXCD VA/PA map ioctls on --ioctl-dev (default %s). The mmap offset\n"
		"is used as the physical address passed to the KMD. Use --system for CPU memory.\n",
		prog, prog, prog, prog, DEFAULT_DEV_PATH, DEFAULT_CTL_DEV,
		DEFAULT_MXCD_DEV);
}

static int is_command(const char *arg)
{
	return !strcmp(arg, "read") || !strcmp(arg, "write") ||
	       !strcmp(arg, "read64") || !strcmp(arg, "write64");
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

static int xioctl(int fd, unsigned long request, void *arg, const char *name)
{
	if (ioctl(fd, request, arg) == 0)
		return 0;

	fprintf(stderr, "%s failed: %s\n", name, strerror(errno));
	return -1;
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

static int register_mapping(const struct options *opt, int ioctl_fd,
			    void *map_addr, uint64_t map_offset,
			    uint64_t map_size)
{
	struct mxcd_ioctl_va_pa_map_args map = { 0 };
	struct mxcd_ioctl_va_to_pa_args va_to_pa = { 0 };

	map.va_addr = (uint64_t)(uintptr_t)map_addr;
	map.pa_addr = map_offset;
	map.size = map_size;
	map.gpu_id = opt->gpu_id;
	if (opt->system_mem)
		map.map_flags |= MXCD_IOC_VA_PA_MAP_FLAGS_SYSTEM;

	if (xioctl(ioctl_fd, MXCD_IOC_VA_PA_MAP, &map, "VA_PA_MAP"))
		return -1;

	va_to_pa.gpu_id = map.gpu_id;
	va_to_pa.va_addr = map.va_addr;
	if (xioctl(ioctl_fd, MXCD_IOC_VA_TO_PA, &va_to_pa, "VA_TO_PA")) {
		struct mxcd_ioctl_va_pa_unmap_args unmap = { 0 };

		unmap.va_addr = map.va_addr;
		unmap.size = map.size;
		unmap.gpu_id = map.gpu_id;
		unmap.map_flags = map.map_flags;
		xioctl(ioctl_fd, MXCD_IOC_VA_PA_UNMAP, &unmap, "VA_PA_UNMAP");
		return -1;
	}

	printf("registered va 0x%llx -> pa 0x%llx, expected pa 0x%llx\n",
	       (unsigned long long)map.va_addr,
	       (unsigned long long)va_to_pa.pa_addr,
	       (unsigned long long)map.pa_addr);
	if (va_to_pa.pa_addr != map.pa_addr) {
		struct mxcd_ioctl_va_pa_unmap_args unmap = { 0 };

		fprintf(stderr, "verify failed: mapped pa mismatch\n");
		unmap.va_addr = map.va_addr;
		unmap.size = map.size;
		unmap.gpu_id = map.gpu_id;
		unmap.map_flags = map.map_flags;
		xioctl(ioctl_fd, MXCD_IOC_VA_PA_UNMAP, &unmap, "VA_PA_UNMAP");
		return -1;
	}

	return 0;
}

static void unregister_mapping(const struct options *opt, int ioctl_fd,
			       void *map_addr, uint64_t map_size)
{
	struct mxcd_ioctl_va_pa_unmap_args unmap = { 0 };

	unmap.va_addr = (uint64_t)(uintptr_t)map_addr;
	unmap.size = map_size;
	unmap.gpu_id = opt->gpu_id;
	if (opt->system_mem)
		unmap.map_flags |= MXCD_IOC_VA_PA_MAP_FLAGS_SYSTEM;

	if (xioctl(ioctl_fd, MXCD_IOC_VA_PA_UNMAP, &unmap, "VA_PA_UNMAP"))
		exit(EXIT_FAILURE);
}

static void do_gpu_read(const struct options *opt, int fd, int ioctl_fd,
			uint64_t offset, uint64_t size)
{
	uint64_t map_offset;
	uint64_t map_size;
	uint64_t offset_in_map;
	void *map_addr;
	void *d_buf = NULL;
	uint8_t *h_dump;

	map_addr = map_physmem(fd, offset, size, &map_offset, &map_size,
			       &offset_in_map);
	if (register_mapping(opt, ioctl_fd, map_addr, map_offset, map_size))
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
	printf("  mmap_offset   = 0x%" PRIx64 "\n", map_offset);
	printf("  mmap_size     = 0x%" PRIx64 " (%" PRIu64 " bytes)\n\n",
	       map_size, map_size);
	dump_hex(h_dump, offset, size);

	CHECK_MC(mcFree(d_buf));
	free(h_dump);
	unregister_mapping(opt, ioctl_fd, map_addr, map_size);
	munmap(map_addr, (size_t)map_size);
}

static void do_gpu_write(const struct options *opt, int fd, int ioctl_fd,
			 uint64_t offset, uint64_t size, uint8_t value)
{
	uint64_t map_offset;
	uint64_t map_size;
	uint64_t offset_in_map;
	void *map_addr;
	void *d_buf = NULL;

	map_addr = map_physmem(fd, offset, size, &map_offset, &map_size,
			       &offset_in_map);
	if (register_mapping(opt, ioctl_fd, map_addr, map_offset, map_size))
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
	printf("  mmap_offset   = 0x%" PRIx64 "\n", map_offset);
	printf("  mmap_size     = 0x%" PRIx64 " (%" PRIu64 " bytes)\n\n",
	       map_size, map_size);
	printf("gpu write done\n");

	CHECK_MC(mcFree(d_buf));
	unregister_mapping(opt, ioctl_fd, map_addr, map_size);
	munmap(map_addr, (size_t)map_size);
}

static void do_gpu_read64(const struct options *opt, int fd, int ioctl_fd,
			      uint64_t offset)
{
	uint64_t value;
	uint64_t map_offset;
	uint64_t map_size;
	uint64_t offset_in_map;
	void *map_addr;
	void *d_buf = NULL;

	if (offset % sizeof(uint64_t)) {
		fprintf(stderr, "read64 offset must be 8-byte aligned\n");
		exit(EXIT_FAILURE);
	}

	map_addr = map_physmem(fd, offset, sizeof(value), &map_offset, &map_size,
			       &offset_in_map);
	if (register_mapping(opt, ioctl_fd, map_addr, map_offset, map_size))
		exit(EXIT_FAILURE);
	CHECK_MC(mcMalloc(&d_buf, sizeof(value)));
	CHECK_MC(mcMemcpy(d_buf, (uint8_t *)map_addr + offset_in_map, sizeof(value),
			  mcMemcpyHostToDevice));
	CHECK_MC(mcMemcpy(&value, d_buf, sizeof(value), mcMemcpyDeviceToHost));
	CHECK_MC(mcDeviceSynchronize());

	printf("GPU READ64:\n");
	printf("  offset = 0x%" PRIx64 "\n", offset);
	printf("  value  = 0x%016" PRIx64 "\n", value);

	CHECK_MC(mcFree(d_buf));
	unregister_mapping(opt, ioctl_fd, map_addr, map_size);
	munmap(map_addr, (size_t)map_size);
}

static void do_gpu_write64(const struct options *opt, int fd, int ioctl_fd,
			   uint64_t offset, uint64_t value)
{
	uint64_t map_offset;
	uint64_t map_size;
	uint64_t offset_in_map;
	void *map_addr;
	void *d_buf = NULL;

	if (offset % sizeof(uint64_t)) {
		fprintf(stderr, "write64 offset must be 8-byte aligned\n");
		exit(EXIT_FAILURE);
	}

	map_addr = map_physmem(fd, offset, sizeof(value), &map_offset, &map_size,
			       &offset_in_map);
	if (register_mapping(opt, ioctl_fd, map_addr, map_offset, map_size))
		exit(EXIT_FAILURE);
	CHECK_MC(mcMalloc(&d_buf, sizeof(value)));
	CHECK_MC(mcMemcpy(d_buf, &value, sizeof(value), mcMemcpyHostToDevice));
	CHECK_MC(mcMemcpy((uint8_t *)map_addr + offset_in_map, d_buf, sizeof(value),
			  mcMemcpyDeviceToHost));
	CHECK_MC(mcDeviceSynchronize());

	printf("GPU WRITE64:\n");
	printf("  offset = 0x%" PRIx64 "\n", offset);
	printf("  value  = 0x%016" PRIx64 "\n", value);
	printf("gpu write64 done\n");

	CHECK_MC(mcFree(d_buf));
	unregister_mapping(opt, ioctl_fd, map_addr, map_size);
	munmap(map_addr, (size_t)map_size);
}

static void parse_options(int argc, char **argv, struct options *opt, int *argi)
{
	opt->gpu = 0;
	opt->gpu_id = 0;
	opt->ioctl_dev = DEFAULT_MXCD_DEV;
	opt->system_mem = 0;
	*argi = 1;

	while (*argi < argc) {
		if (!strcmp(argv[*argi], "--gpu") && *argi + 1 < argc) {
			opt->gpu = (int)parse_u64_arg(argv[*argi + 1], "gpu");
			*argi += 2;
		} else if (!strcmp(argv[*argi], "--ioctl-dev") && *argi + 1 < argc) {
			opt->ioctl_dev = argv[*argi + 1];
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
	const char *dev_path;
	char dev_path_buf[PATH_MAX];
	const char *cmd;
	int argi;
	int fd;
	int ioctl_fd;

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
	dev_path = resolve_dev_path(dev_arg, dev_path_buf, sizeof(dev_path_buf));

	CHECK_MC(mcSetDevice(opt.gpu));
	opt.gpu_id = gpu_index_to_id((uint32_t)opt.gpu);
	ioctl_fd = open(opt.ioctl_dev, O_RDWR | O_CLOEXEC);
	if (ioctl_fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", opt.ioctl_dev, strerror(errno));
		return EXIT_FAILURE;
	}
	fd = open(dev_path, O_RDWR | O_SYNC | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", dev_path, strerror(errno));
		close(ioctl_fd);
		return EXIT_FAILURE;
	}

	if (!strcmp(cmd, "read")) {
		if (argc - argi != 2) {
			usage(argv[0]);
			close(fd);
			close(ioctl_fd);
			return EXIT_FAILURE;
		}
		do_gpu_read(&opt, fd, ioctl_fd, parse_u64_arg(argv[argi], "offset"),
			    parse_u64_arg(argv[argi + 1], "size"));
	} else if (!strcmp(cmd, "write")) {
		if (argc - argi != 3) {
			usage(argv[0]);
			close(fd);
			close(ioctl_fd);
			return EXIT_FAILURE;
		}
		do_gpu_write(&opt, fd, ioctl_fd, parse_u64_arg(argv[argi], "offset"),
			     parse_u64_arg(argv[argi + 1], "size"),
			     parse_u8_arg(argv[argi + 2], "byte_value"));
	} else if (!strcmp(cmd, "read64")) {
		if (argc - argi != 1) {
			usage(argv[0]);
			close(fd);
			close(ioctl_fd);
			return EXIT_FAILURE;
		}
		do_gpu_read64(&opt, fd, ioctl_fd, parse_u64_arg(argv[argi], "offset"));
	} else if (!strcmp(cmd, "write64")) {
		if (argc - argi != 2) {
			usage(argv[0]);
			close(fd);
			close(ioctl_fd);
			return EXIT_FAILURE;
		}
		do_gpu_write64(&opt, fd, ioctl_fd, parse_u64_arg(argv[argi], "offset"),
			       parse_u64_arg(argv[argi + 1], "u64_value"));
	} else {
		fprintf(stderr, "Unknown command: %s\n", cmd);
		usage(argv[0]);
		close(fd);
		close(ioctl_fd);
		return EXIT_FAILURE;
	}

	close(fd);
	close(ioctl_fd);
	return EXIT_SUCCESS;
}
