// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "physmap_ioctl.h"

#define DEFAULT_CTL_DEV "/dev/physmap_ctl"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s create <phys_addr> <size> [UC|WC|WB] [control_device]\n"
		"  %s destroy <id> [control_device]\n"
		"  %s list [control_device]\n"
		"  %s rwtest <device> <write_offset> <write_size> <read_offset> <read_size> [pattern_byte]\n"
		"\n"
		"Numbers accept decimal or 0x-prefixed hexadecimal, plus K/M/G/T/P suffixes. Cache mode defaults to WC.\n",
		prog, prog, prog, prog);
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

static int parse_u64(const char *text, uint64_t *value)
{
	char *end = NULL;
	uint64_t base;
	uint64_t scale;

	errno = 0;
	base = strtoull(text, &end, 0);
	if (errno || end == text || scale_for_suffix(end, &scale))
		return -1;
	if (base > ULLONG_MAX / scale)
		return -1;

	*value = base * scale;
	return 0;
}

static int parse_u32(const char *text, uint32_t *value)
{
	uint64_t tmp;
	if (parse_u64(text, &tmp) || tmp > UINT32_MAX)
		return -1;
	*value = (uint32_t)tmp;
	return 0;
}

static int parse_u8(const char *text, uint8_t *value)
{
	uint64_t tmp;

	if (parse_u64(text, &tmp) || tmp > UINT8_MAX)
		return -1;
	*value = (uint8_t)tmp;
	return 0;
}

static int add_overflows_u64(uint64_t a, uint64_t b, uint64_t *result)
{
	if (a > UINT64_MAX - b)
		return 1;
	*result = a + b;
	return 0;
}

static uint64_t min_nonempty_offset(uint64_t offset_a, uint64_t size_a,
				    uint64_t offset_b, uint64_t size_b)
{
	if (!size_a)
		return offset_b;
	if (!size_b)
		return offset_a;
	return offset_a < offset_b ? offset_a : offset_b;
}

static uint64_t max_nonempty_end(uint64_t end_a, uint64_t size_a,
				 uint64_t end_b, uint64_t size_b)
{
	if (!size_a)
		return end_b;
	if (!size_b)
		return end_a;
	return end_a > end_b ? end_a : end_b;
}

static void fill_pattern(volatile uint8_t *dst, uint64_t size, uint8_t seed)
{
	uint64_t i;

	for (i = 0; i < size; i++)
		dst[i] = (uint8_t)(seed + i);
}

static uint64_t verify_pattern(const volatile uint8_t *src, uint64_t size,
				       uint8_t seed, uint64_t *first_bad,
				       uint8_t *expected, uint8_t *actual)
{
	uint64_t i;
	uint64_t mismatches = 0;

	for (i = 0; i < size; i++) {
		uint8_t want = (uint8_t)(seed + i);
		uint8_t got = src[i];

		if (got == want)
			continue;
		if (!mismatches) {
			*first_bad = i;
			*expected = want;
			*actual = got;
		}
		mismatches++;
	}

	return mismatches;
}

static uint32_t read_checksum(const volatile uint8_t *src, uint64_t size)
{
	uint64_t i;
	uint32_t checksum = 0;

	for (i = 0; i < size; i++)
		checksum = (checksum << 5) - checksum + src[i];

	return checksum;
}

static void flush_mapping_writes(void *mapping, uint64_t map_size)
{
	__sync_synchronize();
	if (msync(mapping, (size_t)map_size, MS_SYNC) && errno != EINVAL)
		fprintf(stderr, "warning: msync failed: %s\n", strerror(errno));
	__sync_synchronize();
}

static void dump_bytes(const volatile uint8_t *src, uint64_t size)
{
	uint64_t i;
	uint64_t dump_size = size > 256 ? 256 : size;

	for (i = 0; i < dump_size; i++) {
		if (i % 16 == 0)
			printf("  %08" PRIx64 ":", i);
		printf(" %02x", src[i]);
		if (i % 16 == 15 || i + 1 == dump_size)
			printf("\n");
	}
	if (size > dump_size)
		printf("  ... truncated, displayed first %" PRIu64 " of %" PRIu64 " bytes\n",
		       dump_size, size);
}

static const char *cache_mode_name(uint32_t mode)
{
	switch (mode) {
	case PHYSMAP_CACHE_UC:
		return "UC";
	case PHYSMAP_CACHE_WC:
		return "WC";
	case PHYSMAP_CACHE_WB:
		return "WB";
	case PHYSMAP_CACHE_DEFAULT:
		return "DEFAULT";
	default:
		return "UNKNOWN";
	}
}

static int parse_cache_mode(const char *text, uint32_t *mode)
{
	if (!text || !*text) {
		*mode = PHYSMAP_CACHE_DEFAULT;
		return 0;
	}
	if (!strcasecmp(text, "UC")) {
		*mode = PHYSMAP_CACHE_UC;
		return 0;
	}
	if (!strcasecmp(text, "WC")) {
		*mode = PHYSMAP_CACHE_WC;
		return 0;
	}
	if (!strcasecmp(text, "WB")) {
		*mode = PHYSMAP_CACHE_WB;
		return 0;
	}
	return -1;
}

static int open_control(const char *path)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
	return fd;
}

static int do_create(int argc, char **argv)
{
	const char *ctl_path = argc >= 6 ? argv[5] : DEFAULT_CTL_DEV;
	struct physmap_create_req req = { 0 };
	uint64_t phys_addr;
	uint64_t size;
	int fd;

	if (argc < 4 || argc > 6) {
		usage(argv[0]);
		return 2;
	}
	if (parse_u64(argv[2], &phys_addr) || parse_u64(argv[3], &size)) {
		fprintf(stderr, "invalid phys_addr or size\n");
		return 2;
	}
	req.phys_addr = phys_addr;
	req.size = size;
	if (parse_cache_mode(argc >= 5 ? argv[4] : NULL, &req.cache_mode)) {
		fprintf(stderr, "invalid cache mode: %s\n", argv[4]);
		return 2;
	}

	fd = open_control(ctl_path);
	if (fd < 0)
		return 1;
	if (ioctl(fd, PHYSMAP_IOC_CREATE, &req) < 0) {
		fprintf(stderr, "PHYSMAP_IOC_CREATE failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);

	printf("id=%u\ndev=%s\n", req.id, req.dev_name);
	return 0;
}

static int do_destroy(int argc, char **argv)
{
	const char *ctl_path = argc >= 4 ? argv[3] : DEFAULT_CTL_DEV;
	struct physmap_destroy_req req = { 0 };
	int fd;

	if (argc < 3 || argc > 4) {
		usage(argv[0]);
		return 2;
	}
	if (parse_u32(argv[2], &req.id)) {
		fprintf(stderr, "invalid id\n");
		return 2;
	}

	fd = open_control(ctl_path);
	if (fd < 0)
		return 1;
	if (ioctl(fd, PHYSMAP_IOC_DESTROY, &req) < 0) {
		fprintf(stderr, "PHYSMAP_IOC_DESTROY failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);

	printf("destroyed id=%u\n", req.id);
	return 0;
}

static int do_list(int argc, char **argv)
{
	const char *ctl_path = argc >= 3 ? argv[2] : DEFAULT_CTL_DEV;
	struct physmap_list_req req = { 0 };
	uint32_t count;
	uint32_t i;
	uint64_t end_addr;
	int fd;

	if (argc > 3) {
		usage(argv[0]);
		return 2;
	}

	fd = open_control(ctl_path);
	if (fd < 0)
		return 1;
	if (ioctl(fd, PHYSMAP_IOC_LIST, &req) < 0) {
		fprintf(stderr, "PHYSMAP_IOC_LIST failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);

	count = req.count > PHYSMAP_MAX_MAPPINGS ? PHYSMAP_MAX_MAPPINGS : req.count;
	printf("%-4s %-16s %-18s %-18s %-18s %-6s %-5s\n",
	       "ID", "DEV", "PHYS_ADDR", "SIZE", "END_ADDR", "CACHE", "REFS");
	for (i = 0; i < count; i++) {
		end_addr = req.entries[i].size ?
			req.entries[i].phys_addr + req.entries[i].size - 1 :
			req.entries[i].phys_addr;
		printf("%-4u %-16s 0x%016" PRIx64 " 0x%016" PRIx64
		       " 0x%016" PRIx64 " %-6s %-5u\n",
		       req.entries[i].id, req.entries[i].dev_name,
		       (uint64_t)req.entries[i].phys_addr,
		       (uint64_t)req.entries[i].size, end_addr,
		       cache_mode_name(req.entries[i].cache_mode),
		       req.entries[i].ref_count);
	}

	return 0;
}

static int do_rwtest(int argc, char **argv)
{
	const char *dev_path;
	uint64_t write_offset;
	uint64_t write_size;
	uint64_t read_offset;
	uint64_t read_size;
	uint64_t write_end;
	uint64_t read_end;
	uint64_t map_first;
	uint64_t map_last;
	uint64_t map_offset;
	uint64_t map_size;
	uint64_t unaligned_map_size;
	uint64_t rounded_map_size;
	long page_size;
	uint8_t pattern = 0xa5;
	void *mapping;
	volatile uint8_t *base;
	uint64_t first_bad;
	uint64_t mismatches;
	uint8_t expected;
	uint8_t actual;
	uint32_t checksum;
	int ret = 0;
	int fd;

	if (argc < 7 || argc > 8) {
		usage(argv[0]);
		return 2;
	}

	dev_path = argv[2];
	if (parse_u64(argv[3], &write_offset) || parse_u64(argv[4], &write_size) ||
	    parse_u64(argv[5], &read_offset) || parse_u64(argv[6], &read_size)) {
		fprintf(stderr, "invalid rwtest offset or size\n");
		return 2;
	}
	if (argc == 8 && parse_u8(argv[7], &pattern)) {
		fprintf(stderr, "invalid pattern_byte\n");
		return 2;
	}
	if (!write_size && !read_size) {
		fprintf(stderr, "write_size and read_size cannot both be zero\n");
		return 2;
	}
	if (add_overflows_u64(write_offset, write_size, &write_end) ||
	    add_overflows_u64(read_offset, read_size, &read_end)) {
		fprintf(stderr, "rwtest offset plus size overflows\n");
		return 2;
	}

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) {
		fprintf(stderr, "failed to get page size\n");
		return 1;
	}

	map_first = min_nonempty_offset(write_offset, write_size, read_offset, read_size);
	map_last = max_nonempty_end(write_end, write_size, read_end, read_size);
	map_offset = (map_first / (uint64_t)page_size) * (uint64_t)page_size;
	unaligned_map_size = map_last - map_offset;
	if (add_overflows_u64(unaligned_map_size, (uint64_t)page_size - 1,
			      &rounded_map_size) || rounded_map_size > SIZE_MAX) {
		fprintf(stderr, "rwtest mmap size is too large\n");
		return 2;
	}
	map_size = (rounded_map_size / (uint64_t)page_size) * (uint64_t)page_size;

	fd = open(dev_path, O_RDWR | O_SYNC | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", dev_path, strerror(errno));
		return 1;
	}

	mapping = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		       (off_t)map_offset);
	if (mapping == MAP_FAILED) {
		fprintf(stderr, "mmap %s offset=0x%" PRIx64 " size=0x%" PRIx64
			" failed: %s\n", dev_path, map_offset, map_size, strerror(errno));
		close(fd);
		return 1;
	}

	base = (volatile uint8_t *)mapping;
	if (write_size) {
		volatile uint8_t *write_ptr = base + (write_offset - map_offset);

		fill_pattern(write_ptr, write_size, pattern);
		flush_mapping_writes(mapping, map_size);
		printf("wrote %" PRIu64 " bytes at offset 0x%" PRIx64
		       " using pattern seed 0x%02x\n",
		       write_size, write_offset, pattern);

		mismatches = verify_pattern(write_ptr, write_size, pattern, &first_bad,
					    &expected, &actual);
		if (mismatches) {
			fprintf(stderr,
				"write verify failed: %" PRIu64 " mismatches, "
				"first at offset 0x%" PRIx64
				" expected=0x%02x actual=0x%02x\n",
				mismatches, write_offset + first_bad, expected, actual);
			ret = 1;
		} else {
			printf("write verify passed for %" PRIu64 " bytes at offset 0x%"
			       PRIx64 "\n", write_size, write_offset);
		}
	}
	if (read_size) {
		checksum = read_checksum(base + (read_offset - map_offset), read_size);
		printf("read %" PRIu64 " bytes at offset 0x%" PRIx64
		       ", checksum=0x%08" PRIx32 "\n",
		       read_size, read_offset, checksum);
		dump_bytes(base + (read_offset - map_offset), read_size);
	}

	munmap(mapping, map_size);
	close(fd);
	return ret;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "create"))
		return do_create(argc, argv);
	if (!strcmp(argv[1], "destroy"))
		return do_destroy(argc, argv);
	if (!strcmp(argv[1], "list"))
		return do_list(argc, argv);
	if (!strcmp(argv[1], "rwtest"))
		return do_rwtest(argc, argv);

	usage(argv[0]);
	return 2;
}
