/* SPDX-License-Identifier: MIT */
/* Declarations only. */
#ifndef LOOPHOLE_LIBDRM_VAMGR_SYS_MMAN_H
#define LOOPHOLE_LIBDRM_VAMGR_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define MAP_SHARED 0x01
#define MAP_FAILED ((void *)-1)

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);

#endif
