// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "physmap_ioctl.h"

#define DEFAULT_CTL_DEV "/dev/physmap_ctl"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s create <phys_addr> <size> [UC|WC|WB] [control_device]\n"
		"  %s destroy <id> [control_device]\n"
		"\n"
		"Numbers accept decimal or 0x-prefixed hexadecimal, plus K/M/G/T/P suffixes. Cache mode defaults to WC.\n",
		prog, prog);
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

	usage(argv[0]);
	return 2;
}
