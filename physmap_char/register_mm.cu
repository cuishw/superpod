// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <mc_runtime.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEFAULT_SIZE_BYTES (64ULL * 1024ULL * 1024ULL)
#define DEFAULT_PAGE_SIZE 4096UL
#define DEFAULT_TOUCH_VALUE 0x5aU

#define CHECK_MC(call)                                                       \
	do {                                                                     \
		mcError_t err__ = (call);                                           \
		if (err__ != mcSuccess) {                                          \
			fprintf(stderr, "MC error %s:%d: %s\n", __FILE__, __LINE__,  \
				mcGetErrorString(err__));                                \
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

enum backing_mode {
	BACKING_SYSTEM,
	BACKING_MMAP_ANON,
	BACKING_MMAP_FILE,
};

enum register_mode {
	REGISTER_NORMAL,
	REGISTER_MMIO,
};

struct options {
	int gpu;
	size_t size;
	uint64_t offset;
	enum backing_mode backing;
	enum register_mode register_mode;
	const char *path;
	int no_touch;
};

static volatile sig_atomic_t stop_requested;

static void handle_stop_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
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
		if (suffix[2] && suffix[2] != 'b' && suffix[2] != 'B')
			return -1;
		if (suffix[2] && suffix[3])
			return -1;
	} else if (suffix[1] && suffix[1] != 'b' && suffix[1] != 'B') {
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

static long get_page_size(void)
{
	long page_size = sysconf(_SC_PAGESIZE);

	return page_size > 0 ? page_size : DEFAULT_PAGE_SIZE;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s [--gpu ID] [--size BYTES] [--backing system|mmap|file]\n"
		"     [--path PATH] [--offset BYTES] [--register normal|mmio] [--no-touch]\n"
		"\n"
		"Register host memory with MUSA and keep the process alive until Ctrl-C/SIGTERM.\n"
		"\n"
		"Options:\n"
		"  --gpu ID             Muxi GPU id, default 0\n"
		"  --size BYTES         registration size, default 64M; accepts K/M/G/T suffixes\n"
		"  --backing system     aligned system malloc memory, default\n"
		"  --backing mmap       anonymous mmap memory\n"
		"  --backing file       file/device/BAR mmap memory, requires --path\n"
		"  --path PATH          file, device, or PCI resource path for --backing file\n"
		"  --offset BYTES       page-aligned mmap offset for --backing file, default 0\n"
		"  --register normal    mcHostRegisterDefault, default\n"
		"  --register mmio      mcHostRegisterIoMemory for MMIO/BAR/physmap mappings\n"
		"  --mmio               shortcut for --register mmio\n"
		"  --no-touch           do not write the host buffer before registration\n"
		"\n"
		"Examples:\n"
		"  %s --gpu 0 --size 1G --backing system\n"
		"  %s --gpu 0 --size 1G --backing mmap --register normal\n"
		"  %s --gpu 0 --size 256M --backing file --path /dev/physmap0 --register mmio\n"
		"  %s --gpu 0 --size 1G --backing file --path /sys/bus/pci/devices/0000:86:00.0/resource2 --offset 2G --mmio\n",
		prog, prog, prog, prog, prog);
}

static void parse_args(int argc, char **argv, struct options *opt)
{
	opt->gpu = 0;
	opt->size = DEFAULT_SIZE_BYTES;
	opt->offset = 0;
	opt->backing = BACKING_SYSTEM;
	opt->register_mode = REGISTER_NORMAL;
	opt->path = NULL;
	opt->no_touch = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--gpu") && i + 1 < argc) {
			opt->gpu = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			uint64_t value = parse_u64_arg(argv[++i], "--size");
			if (value == 0 || value > SIZE_MAX) {
				fprintf(stderr, "--size must be in range 1..%zu\n", (size_t)SIZE_MAX);
				exit(EXIT_FAILURE);
			}
			opt->size = (size_t)value;
		} else if (!strcmp(argv[i], "--offset") && i + 1 < argc) {
			opt->offset = parse_u64_arg(argv[++i], "--offset");
		} else if (!strcmp(argv[i], "--backing") && i + 1 < argc) {
			i++;
			if (!strcmp(argv[i], "system"))
				opt->backing = BACKING_SYSTEM;
			else if (!strcmp(argv[i], "mmap"))
				opt->backing = BACKING_MMAP_ANON;
			else if (!strcmp(argv[i], "file"))
				opt->backing = BACKING_MMAP_FILE;
			else {
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
		} else if (!strcmp(argv[i], "--path") && i + 1 < argc) {
			opt->path = argv[++i];
		} else if (!strcmp(argv[i], "--register") && i + 1 < argc) {
			i++;
			if (!strcmp(argv[i], "normal") || !strcmp(argv[i], "default"))
				opt->register_mode = REGISTER_NORMAL;
			else if (!strcmp(argv[i], "mmio") || !strcmp(argv[i], "io"))
				opt->register_mode = REGISTER_MMIO;
			else {
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
		} else if (!strcmp(argv[i], "--mmio")) {
			opt->register_mode = REGISTER_MMIO;
		} else if (!strcmp(argv[i], "--no-touch")) {
			opt->no_touch = 1;
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		} else {
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (opt->backing != BACKING_MMAP_FILE && opt->path) {
		fprintf(stderr, "--path is only valid with --backing file\n");
		exit(EXIT_FAILURE);
	}
	if (opt->backing != BACKING_MMAP_FILE && opt->offset != 0) {
		fprintf(stderr, "--offset is only valid with --backing file\n");
		exit(EXIT_FAILURE);
	}
	if (opt->backing == BACKING_MMAP_FILE && !opt->path) {
		fprintf(stderr, "--backing file requires --path\n");
		exit(EXIT_FAILURE);
	}
	if (opt->backing == BACKING_MMAP_FILE &&
	    opt->offset % (uint64_t)get_page_size() != 0) {
		fprintf(stderr, "--offset must be page aligned\n");
		exit(EXIT_FAILURE);
	}
	if (opt->backing == BACKING_SYSTEM && opt->register_mode == REGISTER_MMIO) {
		fprintf(stderr, "--register mmio is intended for mmap'ed MMIO memory, not system malloc memory\n");
		exit(EXIT_FAILURE);
	}
}

static const char *backing_name(enum backing_mode backing)
{
	switch (backing) {
	case BACKING_SYSTEM:
		return "system malloc";
	case BACKING_MMAP_ANON:
		return "anonymous mmap";
	case BACKING_MMAP_FILE:
		return "file/device mmap";
	default:
		return "unknown";
	}
}

static void touch_buffer(uint8_t *buf, size_t size)
{
	long page_size = get_page_size();

	for (size_t off = 0; off < size; off += (size_t)page_size)
		buf[off] = (uint8_t)(DEFAULT_TOUCH_VALUE + off);
	if (size > 0)
		buf[size - 1] = DEFAULT_TOUCH_VALUE;
}

static void *alloc_mapping(const struct options *opt, int *fd_out)
{
	void *addr = NULL;
	int fd = -1;

	*fd_out = -1;
	if (opt->backing == BACKING_SYSTEM) {
		long page_size = get_page_size();

		if (posix_memalign(&addr, (size_t)page_size, opt->size) != 0) {
			fprintf(stderr, "posix_memalign failed\n");
			exit(EXIT_FAILURE);
		}
	} else if (opt->backing == BACKING_MMAP_ANON) {
		addr = mmap(NULL, opt->size, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		CHECK_SYS(addr == MAP_FAILED, "mmap anonymous");
	} else {
		if (opt->offset > (uint64_t)LLONG_MAX) {
			fprintf(stderr, "--offset is too large for off_t: 0x%llx\n",
				(unsigned long long)opt->offset);
			exit(EXIT_FAILURE);
		}
		fd = open(opt->path, O_RDWR | O_SYNC);
		CHECK_SYS(fd < 0, "open file/device");
		addr = mmap(NULL, opt->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			    (off_t)opt->offset);
		CHECK_SYS(addr == MAP_FAILED, "mmap file/device");
		*fd_out = fd;
	}

	if (!opt->no_touch && opt->backing != BACKING_MMAP_FILE)
		touch_buffer((uint8_t *)addr, opt->size);

	return addr;
}

static void free_mapping(const struct options *opt, void *addr, int fd)
{
	if (!addr)
		return;
	if (opt->backing == BACKING_SYSTEM)
		free(addr);
	else
		munmap(addr, opt->size);
	if (fd >= 0)
		close(fd);
}

int main(int argc, char **argv)
{
	struct options opt;
	mcDeviceProp_t prop;
	void *addr;
	int fd = -1;
	unsigned int flags;

	parse_args(argc, argv, &opt);
	signal(SIGINT, handle_stop_signal);
	signal(SIGTERM, handle_stop_signal);

	CHECK_MC(mcSetDevice(opt.gpu));
	CHECK_MC(mcGetDeviceProperties(&prop, opt.gpu));
	addr = alloc_mapping(&opt, &fd);
	flags = opt.register_mode == REGISTER_MMIO ? mcHostRegisterIoMemory :
		mcHostRegisterDefault;
	CHECK_MC(mcHostRegister(addr, opt.size, flags));

	printf("MUSA device       : %d\n", opt.gpu);
	printf("MUSA GPU name     : %s\n", prop.name);
	printf("backing           : %s\n", backing_name(opt.backing));
	if (opt.backing == BACKING_MMAP_FILE) {
		printf("path              : %s\n", opt.path);
		printf("mmap offset       : 0x%llx\n", (unsigned long long)opt.offset);
	}
	printf("host address      : %p\n", addr);
	printf("register size     : %zu bytes (%.2f MiB)\n", opt.size,
	       (double)opt.size / 1024.0 / 1024.0);
	printf("register flag     : %s\n",
	       opt.register_mode == REGISTER_MMIO ? "mcHostRegisterIoMemory" :
						       "mcHostRegisterDefault");
	printf("status            : registered; waiting until Ctrl-C/SIGTERM\n");
	fflush(stdout);

	while (!stop_requested)
		pause();

	printf("status            : unregistering\n");
	CHECK_MC(mcHostUnregister(addr));
	free_mapping(&opt, addr, fd);
	printf("status            : done\n");
	return 0;
}
