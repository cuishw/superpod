// SPDX-License-Identifier: MIT
#include <mc_runtime.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_MIN_CHUNK_MIB 1ULL
#define DEFAULT_PATTERN 0xbb

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
	int device;
	int all_devices;
	size_t reserve_mib;
	size_t min_chunk_mib;
};

struct allocation {
	void *ptr;
	size_t size;
};

static void usage(const char *prog)
{
	printf("Usage:\n");
	printf("  %s [--gpu ID|--all] [--reserve-mib MiB] [--min-chunk-mib MiB]\n", prog);
	printf("\n");
	printf("Allocates as much free MUSA GPU memory as possible and writes 0xbb\n");
	printf("to every allocated byte. Default: --all. Compiler: mxcc.\n");
	printf("\n");
	printf("Examples:\n");
	printf("  %s --all\n", prog);
	printf("  %s --gpu 0\n", prog);
	printf("  %s --gpu 0 --reserve-mib 256 --min-chunk-mib 4\n", prog);
}

static size_t parse_size_arg(const char *text, const char *name)
{
	char *end = NULL;
	unsigned long long value;

	errno = 0;
	value = strtoull(text, &end, 0);
	if (errno || end == text || *end != '\0') {
		fprintf(stderr, "invalid %s: %s\n", name, text);
		exit(EXIT_FAILURE);
	}

	return (size_t)value;
}

static int parse_int_arg(const char *text, const char *name)
{
	size_t value = parse_size_arg(text, name);

	if (value > (size_t)INT32_MAX) {
		fprintf(stderr, "%s is too large: %s\n", name, text);
		exit(EXIT_FAILURE);
	}

	return (int)value;
}

static void parse_args(int argc, char **argv, struct options *opt)
{
	opt->device = 0;
	opt->all_devices = 1;
	opt->reserve_mib = 0;
	opt->min_chunk_mib = DEFAULT_MIN_CHUNK_MIB;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--gpu") && i + 1 < argc) {
			opt->device = parse_int_arg(argv[++i], "gpu");
			opt->all_devices = 0;
		} else if (!strcmp(argv[i], "--all")) {
			opt->all_devices = 1;
		} else if (!strcmp(argv[i], "--reserve-mib") && i + 1 < argc) {
			opt->reserve_mib = parse_size_arg(argv[++i], "reserve-mib");
		} else if (!strcmp(argv[i], "--min-chunk-mib") && i + 1 < argc) {
			opt->min_chunk_mib = parse_size_arg(argv[++i], "min-chunk-mib");
			if (!opt->min_chunk_mib)
				opt->min_chunk_mib = DEFAULT_MIN_CHUNK_MIB;
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		} else {
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}
}

static int append_allocation(struct allocation **allocs, size_t *count,
			     size_t *capacity, void *ptr, size_t size)
{
	struct allocation *new_allocs;
	size_t new_capacity;

	if (*count == *capacity) {
		new_capacity = *capacity ? *capacity * 2 : 64;
		new_allocs = realloc(*allocs, new_capacity * sizeof(**allocs));
		if (!new_allocs) {
			fprintf(stderr, "realloc failed: %s\n", strerror(errno));
			return -1;
		}
		*allocs = new_allocs;
		*capacity = new_capacity;
	}

	(*allocs)[*count].ptr = ptr;
	(*allocs)[*count].size = size;
	(*count)++;
	return 0;
}

static void free_allocations(struct allocation *allocs, size_t count)
{
	for (size_t i = 0; i < count; i++)
		CHECK_MC(mcFree(allocs[i].ptr));
}

static int allocate_and_fill_chunk(struct allocation **allocs, size_t *count,
				   size_t *capacity, size_t chunk,
				   size_t *allocated)
{
	void *ptr = NULL;
	mcError_t err;

	err = mcMalloc(&ptr, chunk);
	if (err != mcSuccess)
		return 0;

	err = mcMemset(ptr, DEFAULT_PATTERN, chunk);
	if (err != mcSuccess) {
		fprintf(stderr, "mcMemset failed: %s\n", mcGetErrorString(err));
		mcFree(ptr);
		return -1;
	}

	if (append_allocation(allocs, count, capacity, ptr, chunk)) {
		mcFree(ptr);
		return -1;
	}

	*allocated += chunk;
	return 1;
}

static int test_one_gpu(int gpu, const struct options *opt)
{
	mcDeviceProp_t prop;
	size_t free_bytes = 0;
	size_t total_bytes = 0;
	size_t reserve_bytes = opt->reserve_mib << 20;
	size_t min_chunk = opt->min_chunk_mib << 20;
	size_t target;
	size_t allocated = 0;
	size_t chunk;
	size_t count = 0;
	size_t capacity = 0;
	struct allocation *allocs = NULL;
	int rc = 0;

	CHECK_MC(mcSetDevice(gpu));
	CHECK_MC(mcGetDeviceProperties(&prop, gpu));
	CHECK_MC(mcMemGetInfo(&free_bytes, &total_bytes));

	target = free_bytes > reserve_bytes ? free_bytes - reserve_bytes : 0;
	chunk = target;

	printf("GPU%d %s: total=%zu MiB free=%zu MiB target=%zu MiB pattern=0x%02x\n",
	       gpu, prop.name, total_bytes >> 20, free_bytes >> 20, target >> 20,
	       DEFAULT_PATTERN);

	while (allocated < target && chunk >= min_chunk) {
		int ret;

		if (chunk > target - allocated)
			chunk = target - allocated;
		ret = allocate_and_fill_chunk(&allocs, &count, &capacity, chunk, &allocated);
		if (ret < 0) {
			rc = -1;
			break;
		}
		if (ret > 0)
			continue;
		chunk /= 2;
	}

	CHECK_MC(mcDeviceSynchronize());
	printf("GPU%d %s: allocated_and_filled=%zu MiB allocations=%zu\n",
	       gpu, prop.name, allocated >> 20, count);
	if (allocated < target)
		printf("GPU%d %s: remaining_unallocated=%zu MiB min_chunk=%zu MiB\n",
		       gpu, prop.name, (target - allocated) >> 20, min_chunk >> 20);

	free_allocations(allocs, count);
	free(allocs);
	return rc;
}

int main(int argc, char **argv)
{
	struct options opt;
	int device_count = 0;
	int rc = EXIT_SUCCESS;

	parse_args(argc, argv, &opt);
	CHECK_MC(mcGetDeviceCount(&device_count));
	if (device_count <= 0) {
		fprintf(stderr, "no MUSA GPU found\n");
		return EXIT_FAILURE;
	}

	if (!opt.all_devices) {
		if (opt.device < 0 || opt.device >= device_count) {
			fprintf(stderr, "gpu index %d out of range, device_count=%d\n",
				opt.device, device_count);
			return EXIT_FAILURE;
		}
		return test_one_gpu(opt.device, &opt) ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	for (int gpu = 0; gpu < device_count; gpu++) {
		if (test_one_gpu(gpu, &opt))
			rc = EXIT_FAILURE;
	}

	return rc;
}
