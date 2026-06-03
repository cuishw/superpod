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
#include <sys/mman.h>
#include <unistd.h>

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

enum register_mode {
	REGISTER_IO,
	REGISTER_DEFAULT,
};

struct options {
	int gpu;
	enum register_mode register_mode;
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s [--gpu ID] [--register io|default] [device] read    <offset> <size>\n"
		"  %s [--gpu ID] [--register io|default] [device] write   <offset> <size> <byte_value>\n"
		"  %s [--gpu ID] [--register io|default] [device] read64  <offset>\n"
		"  %s [--gpu ID] [--register io|default] [device] write64 <offset> <u64_value>\n"
		"\n"
		"If device is omitted, %s is used. The default registration flag is io\n"
		"(mcHostRegisterIoMemory), intended for mmap'ed physical/BAR memory.\n",
		prog, prog, prog, prog, DEFAULT_DEV_PATH);
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

static unsigned int register_flags(const struct options *opt)
{
	return opt->register_mode == REGISTER_IO ?
		mcHostRegisterIoMemory : mcHostRegisterDefault;
}

static void register_mapping(const struct options *opt, void *map_addr,
			     uint64_t map_size)
{
	CHECK_MC(mcHostRegister(map_addr, (size_t)map_size, register_flags(opt)));
}

static void do_gpu_read(const struct options *opt, int fd, uint64_t offset,
			uint64_t size)
{
	uint64_t map_offset;
	uint64_t map_size;
	uint64_t offset_in_map;
	void *map_addr;
	void *d_buf = NULL;
	uint8_t *h_dump;

	map_addr = map_physmem(fd, offset, size, &map_offset, &map_size,
			       &offset_in_map);
	register_mapping(opt, map_addr, map_size);
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
	CHECK_MC(mcHostUnregister(map_addr));
	munmap(map_addr, (size_t)map_size);
}

static void do_gpu_write(const struct options *opt, int fd, uint64_t offset,
			 uint64_t size, uint8_t value)
{
	uint64_t map_offset;
	uint64_t map_size;
	uint64_t offset_in_map;
	void *map_addr;
	void *d_buf = NULL;

	map_addr = map_physmem(fd, offset, size, &map_offset, &map_size,
			       &offset_in_map);
	register_mapping(opt, map_addr, map_size);

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
	CHECK_MC(mcHostUnregister(map_addr));
	munmap(map_addr, (size_t)map_size);
}

static void do_gpu_read64(const struct options *opt, int fd, uint64_t offset)
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
	register_mapping(opt, map_addr, map_size);
	CHECK_MC(mcMalloc(&d_buf, sizeof(value)));
	CHECK_MC(mcMemcpy(d_buf, (uint8_t *)map_addr + offset_in_map, sizeof(value),
			  mcMemcpyHostToDevice));
	CHECK_MC(mcMemcpy(&value, d_buf, sizeof(value), mcMemcpyDeviceToHost));
	CHECK_MC(mcDeviceSynchronize());

	printf("GPU READ64:\n");
	printf("  offset = 0x%" PRIx64 "\n", offset);
	printf("  value  = 0x%016" PRIx64 "\n", value);

	CHECK_MC(mcFree(d_buf));
	CHECK_MC(mcHostUnregister(map_addr));
	munmap(map_addr, (size_t)map_size);
}

static void do_gpu_write64(const struct options *opt, int fd, uint64_t offset,
			   uint64_t value)
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
	register_mapping(opt, map_addr, map_size);
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
	CHECK_MC(mcHostUnregister(map_addr));
	munmap(map_addr, (size_t)map_size);
}

static void parse_options(int argc, char **argv, struct options *opt, int *argi)
{
	opt->gpu = 0;
	opt->register_mode = REGISTER_IO;
	*argi = 1;

	while (*argi < argc) {
		if (!strcmp(argv[*argi], "--gpu") && *argi + 1 < argc) {
			opt->gpu = (int)parse_u64_arg(argv[*argi + 1], "gpu");
			*argi += 2;
		} else if (!strcmp(argv[*argi], "--register") && *argi + 1 < argc) {
			if (!strcmp(argv[*argi + 1], "io"))
				opt->register_mode = REGISTER_IO;
			else if (!strcmp(argv[*argi + 1], "default"))
				opt->register_mode = REGISTER_DEFAULT;
			else {
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			*argi += 2;
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
	const char *dev_path = DEFAULT_DEV_PATH;
	const char *cmd;
	int argi;
	int fd;

	if (argc < 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	parse_options(argc, argv, &opt, &argi);
	if (argi < argc && argv[argi][0] == '/')
		dev_path = argv[argi++];
	if (argi >= argc) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	cmd = argv[argi++];

	CHECK_MC(mcSetDevice(opt.gpu));
	fd = open(dev_path, O_RDWR | O_SYNC | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", dev_path, strerror(errno));
		return EXIT_FAILURE;
	}

	if (!strcmp(cmd, "read")) {
		if (argc - argi != 2) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		do_gpu_read(&opt, fd, parse_u64_arg(argv[argi], "offset"),
			    parse_u64_arg(argv[argi + 1], "size"));
	} else if (!strcmp(cmd, "write")) {
		if (argc - argi != 3) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		do_gpu_write(&opt, fd, parse_u64_arg(argv[argi], "offset"),
			     parse_u64_arg(argv[argi + 1], "size"),
			     parse_u8_arg(argv[argi + 2], "byte_value"));
	} else if (!strcmp(cmd, "read64")) {
		if (argc - argi != 1) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		do_gpu_read64(&opt, fd, parse_u64_arg(argv[argi], "offset"));
	} else if (!strcmp(cmd, "write64")) {
		if (argc - argi != 2) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		do_gpu_write64(&opt, fd, parse_u64_arg(argv[argi], "offset"),
			       parse_u64_arg(argv[argi + 1], "u64_value"));
	} else {
		fprintf(stderr, "Unknown command: %s\n", cmd);
		usage(argv[0]);
		close(fd);
		return EXIT_FAILURE;
	}

	close(fd);
	return EXIT_SUCCESS;
}
