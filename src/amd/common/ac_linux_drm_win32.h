/*
 * Copyright 2026 loophole contributors
 * SPDX-License-Identifier: MIT
 */

/* On Windows ac_drm_device::is_virtio is always true. The native DRM paths
 * of ac_linux_drm.c compile against these stand-ins and never run.
 */

#ifndef AC_LINUX_DRM_WIN32_H
#define AC_LINUX_DRM_WIN32_H

#include <errno.h>

static inline int
drm_ioctl(int fd, unsigned long request, void *arg)
{
   return -ENODEV;
}

static inline int
drm_ioctl_write(int fd, unsigned drm_command_index, void *data, unsigned size)
{
   return -ENODEV;
}

static inline int
drm_ioctl_write_read(int fd, unsigned drm_command_index, void *data, unsigned size)
{
   return -ENODEV;
}

#define amdgpu_device_initialize(fd, major, minor, dev) (-ENODEV)
#define amdgpu_device_deinitialize(dev) (-ENODEV)
#define amdgpu_device_get_fd(dev) (-1)
#define amdgpu_query_sw_info(dev, info, value) (-ENODEV)
#define amdgpu_get_marketing_name(dev) ((const char *)NULL)
#define amdgpu_bo_alloc(dev, req, bo) (-ENODEV)
#define amdgpu_bo_free(bo) (-ENODEV)
#define amdgpu_bo_import(dev, type, handle, result) (-ENODEV)
#define amdgpu_bo_export(bo, type, handle) (-ENODEV)
#define amdgpu_bo_cpu_map(bo, cpu) (-ENODEV)
#define amdgpu_bo_cpu_unmap(bo) (-ENODEV)
#define amdgpu_create_bo_from_user_mem(dev, cpu, size, bo) (-ENODEV)

#define util_sync_provider_drm(fd) ((struct util_sync_provider *)NULL)

#endif
