// SPDX-License-Identifier: GPL-2.0
#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "physmap_ioctl.h"

#define PHYSMAP_CTL_MINOR 0
#define PHYSMAP_FIRST_DATA_MINOR 1
#define PHYSMAP_DEVICE_COUNT (PHYSMAP_MAX_MAPPINGS + 1)
#define PHYSMAP_CTL_NAME "physmap_ctl"
#define PHYSMAP_DATA_NAME "physmap%u"
#define PHYSMAP_MMAP_VA_ALIGN (2UL * 1024 * 1024)

struct physmap_region {
	struct cdev cdev;
	struct device *device;
	atomic_t refs;
	phys_addr_t phys_addr;
	resource_size_t size;
	enum physmap_cache_mode cache_mode;
	char identifier[PHYSMAP_IDENT_LEN];
	unsigned int id;
	bool alive;
};

static dev_t physmap_devt;
static struct class *physmap_class;
static DEFINE_MUTEX(regions_lock);
static struct physmap_region *regions[PHYSMAP_MAX_MAPPINGS];
static DECLARE_BITMAP(id_bitmap, PHYSMAP_MAX_MAPPINGS);
static struct cdev ctl_cdev;
static struct device *ctl_device;

static struct class *physmap_class_create(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	return class_create("physmap");
#else
	return class_create(THIS_MODULE, "physmap");
#endif
}

static pgprot_t physmap_pgprot(pgprot_t prot, enum physmap_cache_mode mode)
{
	switch (mode) {
	case PHYSMAP_CACHE_UC:
		return pgprot_noncached(prot);
	case PHYSMAP_CACHE_WB:
		return prot;
	case PHYSMAP_CACHE_DEFAULT:
	case PHYSMAP_CACHE_WC:
	default:
		return pgprot_writecombine(prot);
	}
}

static bool physmap_valid_mode(__u32 mode)
{
	return mode == PHYSMAP_CACHE_DEFAULT || mode == PHYSMAP_CACHE_UC ||
	       mode == PHYSMAP_CACHE_WC || mode == PHYSMAP_CACHE_WB;
}

static bool physmap_identifier_exists(const char *identifier)
{
	unsigned int id;

	for (id = 0; id < PHYSMAP_MAX_MAPPINGS; id++) {
		if (regions[id] && regions[id]->alive &&
		    !strcmp(regions[id]->identifier, identifier))
			return true;
	}

	return false;
}

static struct physmap_region *physmap_get_region(unsigned int id)
{
	struct physmap_region *region;

	mutex_lock(&regions_lock);
	region = id < PHYSMAP_MAX_MAPPINGS ? regions[id] : NULL;
	if (region && region->alive)
		atomic_inc(&region->refs);
	else
		region = NULL;
	mutex_unlock(&regions_lock);

	return region;
}

static void physmap_put_region(struct physmap_region *region)
{
	if (region && atomic_dec_and_test(&region->refs))
		kfree(region);
}

static int physmap_data_open(struct inode *inode, struct file *file)
{
	struct physmap_region *region;
	unsigned int id = iminor(inode) - PHYSMAP_FIRST_DATA_MINOR;

	region = physmap_get_region(id);
	if (!region)
		return -ENODEV;

	file->private_data = region;
	return 0;
}

static int physmap_data_release(struct inode *inode, struct file *file)
{
	physmap_put_region(file->private_data);
	return 0;
}

static int physmap_data_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct physmap_region *region = file->private_data;
	unsigned long requested = vma->vm_end - vma->vm_start;
	phys_addr_t offset = (phys_addr_t)vma->vm_pgoff << PAGE_SHIFT;
	phys_addr_t paddr;

	if (!region)
		return -ENODEV;
	if (offset > region->size || requested > region->size - offset)
		return -EINVAL;
	if (!IS_ALIGNED(region->phys_addr + offset, PAGE_SIZE) ||
	    !IS_ALIGNED(requested, PAGE_SIZE))
		return -EINVAL;

	paddr = region->phys_addr + offset;
	vma->vm_page_prot = physmap_pgprot(vma->vm_page_prot, region->cache_mode);
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;

	return remap_pfn_range(vma, vma->vm_start, paddr >> PAGE_SHIFT, requested,
				       vma->vm_page_prot);
}

static unsigned long physmap_data_get_unmapped_area(struct file *file,
						    unsigned long addr,
						    unsigned long len,
						    unsigned long pgoff,
						    unsigned long flags)
{
	unsigned long area;
	unsigned long aligned;
	unsigned long search_len;

	if (flags & MAP_FIXED)
		return addr;
	if (len > ULONG_MAX - PHYSMAP_MMAP_VA_ALIGN)
		return -ENOMEM;

	search_len = len + PHYSMAP_MMAP_VA_ALIGN;
	area = current->mm->get_unmapped_area(NULL, addr, search_len, pgoff, flags);
	if (IS_ERR_VALUE(area))
		return area;

	aligned = ALIGN(area, PHYSMAP_MMAP_VA_ALIGN);
	if (aligned < area || aligned > area + search_len - len)
		return -ENOMEM;

	return aligned;
}

static const struct file_operations physmap_data_fops = {
	.owner = THIS_MODULE,
	.open = physmap_data_open,
	.release = physmap_data_release,
	.mmap = physmap_data_mmap,
	.get_unmapped_area = physmap_data_get_unmapped_area,
	.llseek = noop_llseek,
};

static void physmap_region_unregister(struct physmap_region *region)
{
	if (!region)
		return;

	device_destroy(physmap_class, MKDEV(MAJOR(physmap_devt),
						 PHYSMAP_FIRST_DATA_MINOR + region->id));
	cdev_del(&region->cdev);
}

static int physmap_create_region(struct physmap_create_req *req)
{
	struct physmap_region *region;
	unsigned int id;
	dev_t devt;
	phys_addr_t end;
	size_t identifier_len;
	int ret;

	identifier_len = strnlen(req->identifier, sizeof(req->identifier));
	if (!identifier_len || identifier_len == sizeof(req->identifier))
		return -EINVAL;
	if (!req->size || !physmap_valid_mode(req->cache_mode))
		return -EINVAL;
	if (!IS_ALIGNED(req->phys_addr, PAGE_SIZE) ||
	    !IS_ALIGNED(req->size, PAGE_SIZE))
		return -EINVAL;
	if (check_add_overflow((phys_addr_t)req->phys_addr,
			       (phys_addr_t)req->size - 1, &end))
		return -EINVAL;

	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return -ENOMEM;

	mutex_lock(&regions_lock);
	if (physmap_identifier_exists(req->identifier)) {
		mutex_unlock(&regions_lock);
		kfree(region);
		return -EEXIST;
	}
	id = find_first_zero_bit(id_bitmap, PHYSMAP_MAX_MAPPINGS);
	if (id >= PHYSMAP_MAX_MAPPINGS) {
		mutex_unlock(&regions_lock);
		kfree(region);
		return -ENOSPC;
	}
	set_bit(id, id_bitmap);
	region->id = id;
	region->alive = true;
	atomic_set(&region->refs, 1);
	region->phys_addr = (phys_addr_t)req->phys_addr;
	region->size = (resource_size_t)req->size;
	region->cache_mode = req->cache_mode == PHYSMAP_CACHE_DEFAULT ?
		PHYSMAP_CACHE_WC : req->cache_mode;
	strscpy(region->identifier, req->identifier, sizeof(region->identifier));
	regions[id] = region;
	mutex_unlock(&regions_lock);

	devt = MKDEV(MAJOR(physmap_devt), PHYSMAP_FIRST_DATA_MINOR + id);
	cdev_init(&region->cdev, &physmap_data_fops);
	region->cdev.owner = THIS_MODULE;
	ret = cdev_add(&region->cdev, devt, 1);
	if (ret)
		goto err_clear;

	region->device = device_create(physmap_class, NULL, devt, NULL,
					      PHYSMAP_DATA_NAME, id);
	if (IS_ERR(region->device)) {
		ret = PTR_ERR(region->device);
		cdev_del(&region->cdev);
		goto err_clear;
	}

	req->id = id;
	snprintf(req->dev_name, sizeof(req->dev_name), "/dev/" PHYSMAP_DATA_NAME, id);
	return 0;

err_clear:
	mutex_lock(&regions_lock);
	regions[id] = NULL;
	clear_bit(id, id_bitmap);
	region->alive = false;
	mutex_unlock(&regions_lock);
	physmap_put_region(region);
	return ret;
}

static void physmap_fill_list(struct physmap_list_req *req)
{
	struct physmap_region *region;
	unsigned int id;

	memset(req, 0, sizeof(*req));

	mutex_lock(&regions_lock);
	for (id = 0; id < PHYSMAP_MAX_MAPPINGS; id++) {
		region = regions[id];
		if (!region || !region->alive)
			continue;

		req->entries[req->count].phys_addr = region->phys_addr;
		req->entries[req->count].size = region->size;
		req->entries[req->count].cache_mode = region->cache_mode;
		req->entries[req->count].id = region->id;
		req->entries[req->count].ref_count = atomic_read(&region->refs);
		snprintf(req->entries[req->count].dev_name,
			 sizeof(req->entries[req->count].dev_name),
			 "/dev/" PHYSMAP_DATA_NAME, region->id);
		strscpy(req->entries[req->count].identifier, region->identifier,
			sizeof(req->entries[req->count].identifier));
		req->count++;
	}
	mutex_unlock(&regions_lock);
}

static int physmap_destroy_region(unsigned int id)
{
	struct physmap_region *region;

	mutex_lock(&regions_lock);
	if (id >= PHYSMAP_MAX_MAPPINGS || !regions[id]) {
		mutex_unlock(&regions_lock);
		return -ENOENT;
	}
	region = regions[id];
	regions[id] = NULL;
	clear_bit(id, id_bitmap);
	region->alive = false;
	mutex_unlock(&regions_lock);

	physmap_region_unregister(region);
	physmap_put_region(region);
	return 0;
}

static long physmap_ctl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct physmap_create_req create_req;
	struct physmap_destroy_req destroy_req;
	struct physmap_list_req *list_req;
	int ret;

	if (_IOC_TYPE(cmd) != PHYSMAP_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case PHYSMAP_IOC_CREATE:
		if (copy_from_user(&create_req, (void __user *)arg, sizeof(create_req)))
			return -EFAULT;
		ret = physmap_create_region(&create_req);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &create_req, sizeof(create_req))) {
			physmap_destroy_region(create_req.id);
			return -EFAULT;
		}
		return 0;
	case PHYSMAP_IOC_DESTROY:
		if (copy_from_user(&destroy_req, (void __user *)arg, sizeof(destroy_req)))
			return -EFAULT;
		return physmap_destroy_region(destroy_req.id);
	case PHYSMAP_IOC_LIST:
		list_req = kzalloc(sizeof(*list_req), GFP_KERNEL);
		if (!list_req)
			return -ENOMEM;

		physmap_fill_list(list_req);
		ret = copy_to_user((void __user *)arg, list_req, sizeof(*list_req)) ?
			-EFAULT : 0;
		kfree(list_req);
		return ret;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations physmap_ctl_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = physmap_ctl_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = physmap_ctl_ioctl,
#endif
	.llseek = noop_llseek,
};

static int __init physmap_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&physmap_devt, 0, PHYSMAP_DEVICE_COUNT, "physmap");
	if (ret)
		return ret;

	physmap_class = physmap_class_create();
	if (IS_ERR(physmap_class)) {
		ret = PTR_ERR(physmap_class);
		goto err_unregister;
	}

	cdev_init(&ctl_cdev, &physmap_ctl_fops);
	ctl_cdev.owner = THIS_MODULE;
	ret = cdev_add(&ctl_cdev, MKDEV(MAJOR(physmap_devt), PHYSMAP_CTL_MINOR), 1);
	if (ret)
		goto err_class;

	ctl_device = device_create(physmap_class, NULL,
				   MKDEV(MAJOR(physmap_devt), PHYSMAP_CTL_MINOR), NULL,
				   PHYSMAP_CTL_NAME);
	if (IS_ERR(ctl_device)) {
		ret = PTR_ERR(ctl_device);
		goto err_cdev;
	}

	pr_info("physmap: control device /dev/%s ready, default cache mode WC\n",
		PHYSMAP_CTL_NAME);
	return 0;

err_cdev:
	cdev_del(&ctl_cdev);
err_class:
	class_destroy(physmap_class);
err_unregister:
	unregister_chrdev_region(physmap_devt, PHYSMAP_DEVICE_COUNT);
	return ret;
}

static void __exit physmap_exit(void)
{
	unsigned int id;

	device_destroy(physmap_class, MKDEV(MAJOR(physmap_devt), PHYSMAP_CTL_MINOR));
	cdev_del(&ctl_cdev);

	for (id = 0; id < PHYSMAP_MAX_MAPPINGS; id++)
		physmap_destroy_region(id);

	class_destroy(physmap_class);
	unregister_chrdev_region(physmap_devt, PHYSMAP_DEVICE_COUNT);
}

module_init(physmap_init);
module_exit(physmap_exit);

MODULE_DESCRIPTION("Configurable physical memory mmap character devices");
MODULE_AUTHOR("OpenAI");
MODULE_LICENSE("GPL");
