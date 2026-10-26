/* SPDX-License-Identifier: MIT */
/* amdgpu_vamgr.c's pthread subset on SRW locks. */
#ifndef LOOPHOLE_LIBDRM_VAMGR_PTHREAD_H
#define LOOPHOLE_LIBDRM_VAMGR_PTHREAD_H

#include <windows.h>

typedef SRWLOCK pthread_mutex_t;
typedef int pthread_mutexattr_t;

#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

static inline int
pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
   (void)attr;
   InitializeSRWLock(m);
   return 0;
}

static inline int
pthread_mutex_lock(pthread_mutex_t *m)
{
   AcquireSRWLockExclusive(m);
   return 0;
}

static inline int
pthread_mutex_unlock(pthread_mutex_t *m)
{
   ReleaseSRWLockExclusive(m);
   return 0;
}

static inline int
pthread_mutex_destroy(pthread_mutex_t *m)
{
   (void)m;
   return 0;
}

#endif
