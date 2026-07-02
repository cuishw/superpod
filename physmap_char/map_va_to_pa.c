// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include "map_va_to_pa.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DEFAULT_MXCD_DEV "/dev/mxcd"
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

static int xioctl(int fd, unsigned long request, void *arg, const char *name)
{
	if (ioctl(fd, request, arg) == 0)
		return 0;

	fprintf(stderr, "%s failed: %s\n", name, strerror(errno));
	return -1;
}

static int open_mxcd(void)
{
	int fd = open(DEFAULT_MXCD_DEV, O_RDWR | O_CLOEXEC);

	if (fd < 0)
		fprintf(stderr, "open %s failed: %s\n", DEFAULT_MXCD_DEV,
			strerror(errno));
	return fd;
}

int map_va_to_pa(uint32_t gpu_id, uint64_t va_addr, uint64_t pa_addr,
		 uint64_t size, int is_cpu_mem)
{
	struct mxcd_ioctl_va_pa_map_args map = { 0 };
	struct mxcd_ioctl_va_to_pa_args va_to_pa = { 0 };
	int fd = open_mxcd();
	int ret = -1;

	if (fd < 0)
		return -1;

	map.va_addr = va_addr;
	map.pa_addr = pa_addr;
	map.size = size;
	map.gpu_id = gpu_id;
	if (is_cpu_mem)
		map.map_flags |= MXCD_IOC_VA_PA_MAP_FLAGS_SYSTEM;

	if (xioctl(fd, MXCD_IOC_VA_PA_MAP, &map, "VA_PA_MAP"))
		goto out;

	va_to_pa.gpu_id = map.gpu_id;
	va_to_pa.va_addr = map.va_addr;
	if (xioctl(fd, MXCD_IOC_VA_TO_PA, &va_to_pa, "VA_TO_PA")) {
		(void)unmap_va_to_pa(gpu_id, va_addr, size, is_cpu_mem);
		goto out;
	}

	printf("registered va 0x%llx -> pa 0x%llx, expected pa 0x%llx\n",
	       (unsigned long long)map.va_addr,
	       (unsigned long long)va_to_pa.pa_addr,
	       (unsigned long long)map.pa_addr);
	if (va_to_pa.pa_addr != map.pa_addr) {
		fprintf(stderr, "verify failed: mapped pa mismatch\n");
		(void)unmap_va_to_pa(gpu_id, va_addr, size, is_cpu_mem);
		goto out;
	}

	ret = 0;
out:
	close(fd);
	return ret;
}

int unmap_va_to_pa(uint32_t gpu_id, uint64_t va_addr, uint64_t size,
		   int is_cpu_mem)
{
	struct mxcd_ioctl_va_pa_unmap_args unmap = { 0 };
	int fd = open_mxcd();
	int ret;

	if (fd < 0)
		return -1;

	unmap.va_addr = va_addr;
	unmap.size = size;
	unmap.gpu_id = gpu_id;
	if (is_cpu_mem)
		unmap.map_flags |= MXCD_IOC_VA_PA_MAP_FLAGS_SYSTEM;

	ret = xioctl(fd, MXCD_IOC_VA_PA_UNMAP, &unmap, "VA_PA_UNMAP");
	close(fd);
	return ret;
}
