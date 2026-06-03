/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef PHYSMAP_IOCTL_H
#define PHYSMAP_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define PHYSMAP_NAME_LEN 32
#define PHYSMAP_MAX_MAPPINGS 64

/* Users may omit the cache mode to get the driver default: WC. */
enum physmap_cache_mode {
	PHYSMAP_CACHE_DEFAULT = 0,
	PHYSMAP_CACHE_UC = 1,
	PHYSMAP_CACHE_WC = 2,
	PHYSMAP_CACHE_WB = 3,
};

struct physmap_create_req {
	__u64 phys_addr;
	__u64 size;
	__u32 cache_mode;
	__u32 id;
	char dev_name[PHYSMAP_NAME_LEN];
};

struct physmap_destroy_req {
	__u32 id;
	__u32 reserved;
};

struct physmap_list_entry {
	__u64 phys_addr;
	__u64 size;
	__u32 cache_mode;
	__u32 id;
	__u32 ref_count;
	__u32 reserved;
	char dev_name[PHYSMAP_NAME_LEN];
};

struct physmap_list_req {
	__u32 count;
	__u32 reserved;
	struct physmap_list_entry entries[PHYSMAP_MAX_MAPPINGS];
};

#define PHYSMAP_IOC_MAGIC 'p'
#define PHYSMAP_IOC_CREATE _IOWR(PHYSMAP_IOC_MAGIC, 1, struct physmap_create_req)
#define PHYSMAP_IOC_DESTROY _IOW(PHYSMAP_IOC_MAGIC, 2, struct physmap_destroy_req)
#define PHYSMAP_IOC_LIST _IOR(PHYSMAP_IOC_MAGIC, 3, struct physmap_list_req)

#endif /* PHYSMAP_IOCTL_H */
