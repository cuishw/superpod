// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

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

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s [device] read    <offset> <size>\n"
		"  %s [device] write   <offset> <size> <byte_value>\n"
		"  %s [device] read64  <offset>\n"
		"  %s [device] write64 <offset> <u64_value>\n"
		"\n"
		"If device is omitted, %s is used. Numbers accept decimal, 0x-prefixed\n"
		"hexadecimal, and K/M/G/T/P binary suffixes.\n"
		"\n"
		"Examples:\n"
		"  %s read 0x0 64\n"
		"  %s /dev/physmap1 read 0x1000 256\n"
		"  %s write 0x0 64 0x5a\n"
		"  %s read64 0x1000\n"
		"  %s write64 0x1000 0xdeadbeef12345678\n",
		prog, prog, prog, prog, DEFAULT_DEV_PATH,
		prog, prog, prog, prog, prog);
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

static long get_page_size(void)
{
	long page_size = sysconf(_SC_PAGESIZE);

	if (page_size <= 0)
		return DEFAULT_PAGE_SIZE;
	return page_size;
}

static void dump_hex(const volatile uint8_t *buf, uint64_t base_offset,
		     uint64_t size)
{
	uint64_t i;

	for (i = 0; i < size; i++) {
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
	uint64_t access_end;
	uint64_t map_offset;
	uint64_t offset_in_map;
	uint64_t map_span;
	uint64_t map_size;
	void *map_addr;

	if (!access_size) {
		fprintf(stderr, "access size must be > 0\n");
		exit(EXIT_FAILURE);
	}
	if (add_overflows_u64(target_offset, access_size, &access_end)) {
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

static void do_read(int fd, uint64_t offset, uint64_t size)
{
	uint64_t map_offset;
	uint64_t map_size;
	uint64_t offset_in_map;
	void *map_addr;
	volatile uint8_t *p;

	map_addr = map_physmem(fd, offset, size, &map_offset, &map_size,
			       &offset_in_map);
	p = (volatile uint8_t *)map_addr + offset_in_map;

	printf("READ:\n");
	printf("  target_offset = 0x%" PRIx64 "\n", offset);
	printf("  size          = 0x%" PRIx64 " (%" PRIu64 " bytes)\n", size, size);
	printf("  mmap_offset   = 0x%" PRIx64 "\n", map_offset);
	printf("  mmap_size     = 0x%" PRIx64 " (%" PRIu64 " bytes)\n\n",
	       map_size, map_size);
	dump_hex(p, offset, size);

	munmap(map_addr, (size_t)map_size);
}

static void do_write(int fd, uint64_t offset, uint64_t size, uint8_t value)
{
	uint64_t map_offset;
	uint64_t map_size;
	uint64_t offset_in_map;
	void *map_addr;
	volatile uint8_t *p;
	uint64_t i;

	map_addr = map_physmem(fd, offset, size, &map_offset, &map_size,
			       &offset_in_map);
	p = (volatile uint8_t *)map_addr + offset_in_map;

	printf("WRITE:\n");
	printf("  target_offset = 0x%" PRIx64 "\n", offset);
	printf("  size          = 0x%" PRIx64 " (%" PRIu64 " bytes)\n", size, size);
	printf("  value         = 0x%02x\n", value);
	printf("  mmap_offset   = 0x%" PRIx64 "\n", map_offset);
	printf("  mmap_size     = 0x%" PRIx64 " (%" PRIu64 " bytes)\n\n",
	       map_size, map_size);

	for (i = 0; i < size; i++)
		p[i] = value;
	__sync_synchronize();
	printf("write done\n");

	munmap(map_addr, (size_t)map_size);
}

static void do_read64(int fd, uint64_t offset)
{
	uint64_t map_offset;
	uint64_t map_size;
	uint64_t offset_in_map;
	void *map_addr;
	volatile uint64_t *p;
	uint64_t value;

	if (offset % sizeof(uint64_t)) {
		fprintf(stderr, "read64 offset must be 8-byte aligned\n");
		exit(EXIT_FAILURE);
	}

	map_addr = map_physmem(fd, offset, sizeof(uint64_t), &map_offset, &map_size,
			       &offset_in_map);
	p = (volatile uint64_t *)((volatile uint8_t *)map_addr + offset_in_map);
	value = *p;

	printf("READ64:\n");
	printf("  offset = 0x%" PRIx64 "\n", offset);
	printf("  value  = 0x%016" PRIx64 "\n", value);

	munmap(map_addr, (size_t)map_size);
}

static void do_write64(int fd, uint64_t offset, uint64_t value)
{
	uint64_t map_offset;
	uint64_t map_size;
	uint64_t offset_in_map;
	void *map_addr;
	volatile uint64_t *p;

	if (offset % sizeof(uint64_t)) {
		fprintf(stderr, "write64 offset must be 8-byte aligned\n");
		exit(EXIT_FAILURE);
	}

	map_addr = map_physmem(fd, offset, sizeof(uint64_t), &map_offset, &map_size,
			       &offset_in_map);
	p = (volatile uint64_t *)((volatile uint8_t *)map_addr + offset_in_map);

	printf("WRITE64:\n");
	printf("  offset = 0x%" PRIx64 "\n", offset);
	printf("  value  = 0x%016" PRIx64 "\n", value);

	*p = value;
	__sync_synchronize();
	printf("write64 done\n");

	munmap(map_addr, (size_t)map_size);
}

int main(int argc, char **argv)
{
	const char *dev_path = DEFAULT_DEV_PATH;
	const char *cmd;
	int argi = 1;
	int fd;

	if (argc < 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	if (argv[argi][0] == '/')
		dev_path = argv[argi++];
	if (argi >= argc) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	cmd = argv[argi++];

	fd = open(dev_path, O_RDWR | O_SYNC | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", dev_path, strerror(errno));
		return EXIT_FAILURE;
	}

	if (!strcmp(cmd, "read")) {
		uint64_t offset;
		uint64_t size;

		if (argc - argi != 2) {
			usage(argv[0]);
			close(fd);
			return EXIT_FAILURE;
		}
		offset = parse_u64_arg(argv[argi], "offset");
		size = parse_u64_arg(argv[argi + 1], "size");
		do_read(fd, offset, size);
	} else if (!strcmp(cmd, "write")) {
		uint64_t offset;
		uint64_t size;
		uint8_t value;

		if (argc - argi != 3) {
			usage(argv[0]);
			close(fd);
			return EXIT_FAILURE;
		}
		offset = parse_u64_arg(argv[argi], "offset");
		size = parse_u64_arg(argv[argi + 1], "size");
		value = parse_u8_arg(argv[argi + 2], "byte_value");
		do_write(fd, offset, size, value);
	} else if (!strcmp(cmd, "read64")) {
		uint64_t offset;

		if (argc - argi != 1) {
			usage(argv[0]);
			close(fd);
			return EXIT_FAILURE;
		}
		offset = parse_u64_arg(argv[argi], "offset");
		do_read64(fd, offset);
	} else if (!strcmp(cmd, "write64")) {
		uint64_t offset;
		uint64_t value;

		if (argc - argi != 2) {
			usage(argv[0]);
			close(fd);
			return EXIT_FAILURE;
		}
		offset = parse_u64_arg(argv[argi], "offset");
		value = parse_u64_arg(argv[argi + 1], "u64_value");
		do_write64(fd, offset, value);
	} else {
		fprintf(stderr, "Unknown command: %s\n", cmd);
		usage(argv[0]);
		close(fd);
		return EXIT_FAILURE;
	}

	close(fd);
	return EXIT_SUCCESS;
}
