// SPDX-License-Identifier: MIT
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_MIN_CHUNK (1ULL << 20)
#define DEFAULT_PATTERN 0xbbU

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef uint64_t CUdeviceptr;

#define CUDA_SUCCESS 0

typedef CUresult (*cuInit_fn)(unsigned int flags);
typedef CUresult (*cuDeviceGetCount_fn)(int *count);
typedef CUresult (*cuDeviceGet_fn)(CUdevice *device, int ordinal);
typedef CUresult (*cuDeviceGetName_fn)(char *name, int len, CUdevice dev);
typedef CUresult (*cuDeviceTotalMem_fn)(size_t *bytes, CUdevice dev);
typedef CUresult (*cuCtxCreate_fn)(CUcontext *pctx, unsigned int flags, CUdevice dev);
typedef CUresult (*cuCtxDestroy_fn)(CUcontext ctx);
typedef CUresult (*cuMemGetInfo_fn)(size_t *free_bytes, size_t *total_bytes);
typedef CUresult (*cuMemAlloc_fn)(CUdeviceptr *dptr, size_t bytesize);
typedef CUresult (*cuMemFree_fn)(CUdeviceptr dptr);
typedef CUresult (*cuMemsetD8_fn)(CUdeviceptr dst_device, unsigned char uc, size_t n);
typedef CUresult (*cuCtxSynchronize_fn)(void);
typedef CUresult (*cuGetErrorName_fn)(CUresult error, const char **pstr);
typedef CUresult (*cuGetErrorString_fn)(CUresult error, const char **pstr);

struct cuda_api {
	void *handle;
	cuInit_fn cuInit;
	cuDeviceGetCount_fn cuDeviceGetCount;
	cuDeviceGet_fn cuDeviceGet;
	cuDeviceGetName_fn cuDeviceGetName;
	cuDeviceTotalMem_fn cuDeviceTotalMem;
	cuCtxCreate_fn cuCtxCreate;
	cuCtxDestroy_fn cuCtxDestroy;
	cuMemGetInfo_fn cuMemGetInfo;
	cuMemAlloc_fn cuMemAlloc;
	cuMemFree_fn cuMemFree;
	cuMemsetD8_fn cuMemsetD8;
	cuCtxSynchronize_fn cuCtxSynchronize;
	cuGetErrorName_fn cuGetErrorName;
	cuGetErrorString_fn cuGetErrorString;
};

struct allocation {
	CUdeviceptr ptr;
	size_t size;
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s [--device <index>] [--reserve-mib <MiB>] [--min-chunk-mib <MiB>]\n"
		"\n"
		"Allocates as much free GPU memory as possible and writes byte pattern 0xbb\n"
		"to every allocated byte. Without --device, all GPUs are tested.\n",
		prog);
}

static uint64_t parse_u64_arg(const char *text, const char *name)
{
	char *end = NULL;
	uint64_t value;

	errno = 0;
	value = strtoull(text, &end, 0);
	if (errno || end == text || *end != '\0') {
		fprintf(stderr, "invalid %s: %s\n", name, text);
		exit(EXIT_FAILURE);
	}

	return value;
}

static void *load_symbol(void *handle, const char *name)
{
	void *sym;

	dlerror();
	sym = dlsym(handle, name);
	if (!sym)
		fprintf(stderr, "missing CUDA symbol %s: %s\n", name, dlerror());
	return sym;
}

static void *load_first_symbol(void *handle, const char *name_a, const char *name_b)
{
	void *sym;

	dlerror();
	sym = dlsym(handle, name_a);
	if (sym)
		return sym;
	dlerror();
	sym = dlsym(handle, name_b);
	if (!sym)
		fprintf(stderr, "missing CUDA symbol %s/%s: %s\n", name_a, name_b, dlerror());
	return sym;
}

static int load_cuda(struct cuda_api *api)
{
	memset(api, 0, sizeof(*api));
	api->handle = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!api->handle)
		api->handle = dlopen("libcuda.so", RTLD_NOW | RTLD_LOCAL);
	if (!api->handle) {
		fprintf(stderr, "failed to load libcuda: %s\n", dlerror());
		return -1;
	}

	api->cuInit = (cuInit_fn)load_symbol(api->handle, "cuInit");
	api->cuDeviceGetCount = (cuDeviceGetCount_fn)load_symbol(api->handle, "cuDeviceGetCount");
	api->cuDeviceGet = (cuDeviceGet_fn)load_symbol(api->handle, "cuDeviceGet");
	api->cuDeviceGetName = (cuDeviceGetName_fn)load_symbol(api->handle, "cuDeviceGetName");
	api->cuDeviceTotalMem = (cuDeviceTotalMem_fn)load_first_symbol(api->handle,
								    "cuDeviceTotalMem_v2",
								    "cuDeviceTotalMem");
	api->cuCtxCreate = (cuCtxCreate_fn)load_first_symbol(api->handle,
						       "cuCtxCreate_v2", "cuCtxCreate");
	api->cuCtxDestroy = (cuCtxDestroy_fn)load_first_symbol(api->handle,
							"cuCtxDestroy_v2", "cuCtxDestroy");
	api->cuMemGetInfo = (cuMemGetInfo_fn)load_first_symbol(api->handle,
							"cuMemGetInfo_v2", "cuMemGetInfo");
	api->cuMemAlloc = (cuMemAlloc_fn)load_first_symbol(api->handle,
						      "cuMemAlloc_v2", "cuMemAlloc");
	api->cuMemFree = (cuMemFree_fn)load_first_symbol(api->handle,
						     "cuMemFree_v2", "cuMemFree");
	api->cuMemsetD8 = (cuMemsetD8_fn)load_symbol(api->handle, "cuMemsetD8_v2");
	api->cuCtxSynchronize = (cuCtxSynchronize_fn)load_symbol(api->handle, "cuCtxSynchronize");
	api->cuGetErrorName = (cuGetErrorName_fn)load_symbol(api->handle, "cuGetErrorName");
	api->cuGetErrorString = (cuGetErrorString_fn)load_symbol(api->handle, "cuGetErrorString");

	if (!api->cuInit || !api->cuDeviceGetCount || !api->cuDeviceGet ||
	    !api->cuDeviceGetName || !api->cuDeviceTotalMem || !api->cuCtxCreate ||
	    !api->cuCtxDestroy || !api->cuMemGetInfo || !api->cuMemAlloc ||
	    !api->cuMemFree || !api->cuMemsetD8 || !api->cuCtxSynchronize ||
	    !api->cuGetErrorName || !api->cuGetErrorString)
		return -1;

	return 0;
}

static const char *cuda_error(struct cuda_api *api, CUresult result)
{
	const char *name = NULL;
	const char *desc = NULL;
	static char buf[256];

	if (api->cuGetErrorName(result, &name) != CUDA_SUCCESS)
		name = "UNKNOWN";
	if (api->cuGetErrorString(result, &desc) != CUDA_SUCCESS)
		desc = "no description";
	snprintf(buf, sizeof(buf), "%s: %s", name, desc);
	return buf;
}

static int append_allocation(struct allocation **allocs, size_t *count,
			     size_t *capacity, CUdeviceptr ptr, size_t size)
{
	struct allocation *new_allocs;
	size_t new_capacity;

	if (*count == *capacity) {
		new_capacity = *capacity ? *capacity * 2 : 64;
		new_allocs = realloc(*allocs, new_capacity * sizeof(**allocs));
		if (!new_allocs) {
			perror("realloc");
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

static void free_allocations(struct cuda_api *api, struct allocation *allocs,
			     size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		api->cuMemFree(allocs[i].ptr);
}

static int test_device(struct cuda_api *api, int ordinal, size_t reserve_bytes,
		       size_t min_chunk)
{
	CUdevice device;
	CUcontext ctx = NULL;
	CUdeviceptr ptr = 0;
	CUresult result;
	char name[128] = { 0 };
	size_t free_bytes = 0;
	size_t total_bytes = 0;
	size_t driver_total = 0;
	size_t target;
	size_t chunk;
	size_t allocated = 0;
	size_t alloc_count = 0;
	size_t alloc_capacity = 0;
	struct allocation *allocs = NULL;
	int rc = 0;

	result = api->cuDeviceGet(&device, ordinal);
	if (result != CUDA_SUCCESS) {
		fprintf(stderr, "GPU%d: cuDeviceGet failed: %s\n", ordinal,
			cuda_error(api, result));
		return -1;
	}
	api->cuDeviceGetName(name, sizeof(name), device);
	api->cuDeviceTotalMem(&driver_total, device);

	result = api->cuCtxCreate(&ctx, 0, device);
	if (result != CUDA_SUCCESS) {
		fprintf(stderr, "GPU%d %s: cuCtxCreate failed: %s\n", ordinal, name,
			cuda_error(api, result));
		return -1;
	}

	result = api->cuMemGetInfo(&free_bytes, &total_bytes);
	if (result != CUDA_SUCCESS) {
		fprintf(stderr, "GPU%d %s: cuMemGetInfo failed: %s\n", ordinal, name,
			cuda_error(api, result));
		rc = -1;
		goto out;
	}

	target = free_bytes > reserve_bytes ? free_bytes - reserve_bytes : 0;
	chunk = target;
	printf("GPU%d %s: total=%zu MiB free=%zu MiB target=%zu MiB pattern=0x%02x\n",
	       ordinal, name, driver_total >> 20, free_bytes >> 20, target >> 20,
	       DEFAULT_PATTERN);

	while (chunk >= min_chunk && allocated < target) {
		if (chunk > target - allocated)
			chunk = target - allocated;
		result = api->cuMemAlloc(&ptr, chunk);
		if (result == CUDA_SUCCESS) {
			result = api->cuMemsetD8(ptr, DEFAULT_PATTERN, chunk);
			if (result != CUDA_SUCCESS) {
				fprintf(stderr, "GPU%d: cuMemsetD8 failed: %s\n", ordinal,
					cuda_error(api, result));
				api->cuMemFree(ptr);
				rc = -1;
				break;
			}
			if (append_allocation(&allocs, &alloc_count, &alloc_capacity, ptr,
					      chunk)) {
				api->cuMemFree(ptr);
				rc = -1;
				break;
			}
			allocated += chunk;
			continue;
		}
		chunk /= 2;
	}

	result = api->cuCtxSynchronize();
	if (result != CUDA_SUCCESS) {
		fprintf(stderr, "GPU%d: cuCtxSynchronize failed: %s\n", ordinal,
			cuda_error(api, result));
		rc = -1;
	}

	printf("GPU%d %s: allocated_and_filled=%zu MiB allocations=%zu\n",
	       ordinal, name, allocated >> 20, alloc_count);
	if (allocated < target)
		printf("GPU%d %s: stopped with %zu MiB below target (minimum chunk %zu MiB)\n",
		       ordinal, name, (target - allocated) >> 20, min_chunk >> 20);

out:
	free_allocations(api, allocs, alloc_count);
	free(allocs);
	api->cuCtxDestroy(ctx);
	return rc;
}

int main(int argc, char **argv)
{
	struct cuda_api api;
	int device_count = 0;
	int selected_device = -1;
	size_t reserve_bytes = 0;
	size_t min_chunk = DEFAULT_MIN_CHUNK;
	CUresult result;
	int i;
	int rc = EXIT_SUCCESS;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			selected_device = (int)parse_u64_arg(argv[++i], "device");
		} else if (!strcmp(argv[i], "--reserve-mib") && i + 1 < argc) {
			reserve_bytes = parse_u64_arg(argv[++i], "reserve-mib") << 20;
		} else if (!strcmp(argv[i], "--min-chunk-mib") && i + 1 < argc) {
			min_chunk = parse_u64_arg(argv[++i], "min-chunk-mib") << 20;
			if (!min_chunk)
				min_chunk = DEFAULT_MIN_CHUNK;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return EXIT_SUCCESS;
		} else {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (load_cuda(&api))
		return EXIT_FAILURE;

	result = api.cuInit(0);
	if (result != CUDA_SUCCESS) {
		fprintf(stderr, "cuInit failed: %s\n", cuda_error(&api, result));
		return EXIT_FAILURE;
	}
	result = api.cuDeviceGetCount(&device_count);
	if (result != CUDA_SUCCESS) {
		fprintf(stderr, "cuDeviceGetCount failed: %s\n", cuda_error(&api, result));
		return EXIT_FAILURE;
	}
	if (!device_count) {
		fprintf(stderr, "no CUDA-capable GPU found\n");
		return EXIT_FAILURE;
	}

	if (selected_device >= 0) {
		if (selected_device >= device_count) {
			fprintf(stderr, "device index %d out of range (count=%d)\n",
				selected_device, device_count);
			return EXIT_FAILURE;
		}
		return test_device(&api, selected_device, reserve_bytes, min_chunk) ?
			EXIT_FAILURE : EXIT_SUCCESS;
	}

	for (i = 0; i < device_count; i++) {
		if (test_device(&api, i, reserve_bytes, min_chunk))
			rc = EXIT_FAILURE;
	}

	return rc;
}
