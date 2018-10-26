/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_gem.c
 *
 * (C) Copyright 2012, Red Hat
 * (C) Copyright 2018, Artem Mygaiev
 */

#include "fl2000.h"

#define UDL_BO_CACHEABLE	(1 << 0)
#define UDL_BO_WC		(1 << 1)

static const struct vm_operations_struct udl_gem_vm_ops = {
	.fault = udl_gem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static struct drm_driver fl2000_drm_driver = {

	.gem_free_object = udl_gem_free_object,
	.gem_vm_ops = &udl_gem_vm_ops,

	.dumb_create = fl2000_dumb_create,
	.dumb_map_offset = drm_gem_dumb_map_offset,
	.dumb_destroy = drm_gem_dumb_destroy,

	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,

	.gem_prime_export = drm_gem_prime_import,
	.gem_prime_import = drm_gem_prime_export,

	.gem_prime_get_sg_table = NULL,
	.gem_prime_vmap = NULL,
	.gem_prime_vunmap = NULL,

};


struct udl_gem_object *udl_gem_alloc_object(struct drm_device *dev, size_t size)
{
	struct udl_gem_object *obj;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (obj == NULL)
		return NULL;

	if (drm_gem_object_init(dev, &obj->base, size) != 0) {
		kfree(obj);
		return NULL;
	}

	obj->flags = UDL_BO_CACHEABLE;
	return obj;
}

static int udl_gem_create(struct drm_file *file, struct drm_device *dev,
		u64 size, u32 *handle_p)
{
	struct udl_gem_object *obj;
	int ret;
	u32 handle;

	size = roundup(size, PAGE_SIZE);

	obj = udl_gem_alloc_object(dev, size);
	if (obj == NULL)
		return -ENOMEM;

	ret = drm_gem_handle_create(file, &obj->base, &handle);
	if (ret) {
		drm_gem_object_release(&obj->base);
		kfree(obj);
		return ret;
	}

	drm_gem_object_unreference_unlocked(&obj->base);
	*handle_p = handle;
	return 0;
}

static void update_vm_cache_attr(struct udl_gem_object *obj,
		struct vm_area_struct *vma)
{
	DRM_DEBUG_KMS("flags = 0x%x\n", obj->flags);

	/* non-cacheable as default. */
	if (obj->flags & UDL_BO_CACHEABLE) {
		vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	} else if (obj->flags & UDL_BO_WC) {
		vma->vm_page_prot =
			pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
	} else {
		vma->vm_page_prot =
			pgprot_noncached(vm_get_page_prot(vma->vm_flags));
	}
}

int fl2000_dumb_create(struct drm_file *file, struct drm_device *dev,
		struct drm_mode_create_dumb *args)
{
	args->pitch = args->width * DIV_ROUND_UP(args->bpp, 8);
	args->size = args->pitch * args->height;
	return udl_gem_create(file, dev,
			      args->size, &args->handle);
}

int udl_drm_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int ret;

	ret = drm_gem_mmap(filp, vma);
	if (ret)
		return ret;

	vma->vm_flags &= ~VM_PFNMAP;
	vma->vm_flags |= VM_MIXEDMAP;

	update_vm_cache_attr(to_udl_bo(vma->vm_private_data), vma);

	return ret;
}

int udl_gem_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct udl_gem_object *obj = to_udl_bo(vma->vm_private_data);
	struct page *page;
	unsigned int page_offset;
	int ret = 0;

	page_offset = (vmf->address - vma->vm_start) >>
		PAGE_SHIFT;

	if (!obj->pages)
		return VM_FAULT_SIGBUS;

	page = obj->pages[page_offset];
	ret = vm_insert_page(vma, vmf->address, page);
	switch (ret) {
	case -EAGAIN:
	case 0:
	case -ERESTARTSYS:
		return VM_FAULT_NOPAGE;
	case -ENOMEM:
		return VM_FAULT_OOM;
	default:
		return VM_FAULT_SIGBUS;
	}
}

int udl_gem_get_pages(struct udl_gem_object *obj)
{
	struct page **pages;

	if (obj->pages)
		return 0;

	pages = drm_gem_get_pages(&obj->base);
	if (IS_ERR(pages))
		return PTR_ERR(pages);

	obj->pages = pages;

	return 0;
}

void udl_gem_put_pages(struct udl_gem_object *obj)
{
	if (obj->base.import_attach) {
		drm_free_large(obj->pages);
		obj->pages = NULL;
		return;
	}

	drm_gem_put_pages(&obj->base, obj->pages, false, false);
	obj->pages = NULL;
}

int udl_gem_vmap(struct udl_gem_object *obj)
{
	int page_count = obj->base.size / PAGE_SIZE;
	int ret;

	if (obj->base.import_attach) {
		obj->vmapping = dma_buf_vmap(obj->base.import_attach->dmabuf);
		if (!obj->vmapping)
			return -ENOMEM;
		return 0;
	}

	ret = udl_gem_get_pages(obj);
	if (ret)
		return ret;

	obj->vmapping = vmap(obj->pages, page_count, 0, PAGE_KERNEL);
	if (!obj->vmapping)
		return -ENOMEM;
	return 0;
}

void udl_gem_vunmap(struct udl_gem_object *obj)
{
	if (obj->base.import_attach) {
		dma_buf_vunmap(obj->base.import_attach->dmabuf, obj->vmapping);
		return;
	}

	vunmap(obj->vmapping);

	udl_gem_put_pages(obj);
}

void udl_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct udl_gem_object *obj = to_udl_bo(gem_obj);

	if (obj->vmapping)
		udl_gem_vunmap(obj);

	if (gem_obj->import_attach) {
		drm_prime_gem_destroy(gem_obj, obj->sg);
		put_device(gem_obj->dev->dev);
	}

	if (obj->pages)
		udl_gem_put_pages(obj);

	drm_gem_free_mmap_offset(gem_obj);
}
