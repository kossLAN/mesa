/* SPDX-License-Identifier: MIT */
/* winsock defines some of these for its own ioctls. */
#ifndef LOOPHOLE_COMPAT_SYS_IOCCOM_H
#define LOOPHOLE_COMPAT_SYS_IOCCOM_H

#ifndef IOCPARM_MASK
#define IOCPARM_MASK 0x7f
#endif
#ifndef IOC_VOID
#define IOC_VOID 0x20000000UL
#endif
#ifndef IOC_OUT
#define IOC_OUT 0x40000000UL
#endif
#ifndef IOC_IN
#define IOC_IN 0x80000000UL
#endif
#ifndef IOC_INOUT
#define IOC_INOUT (IOC_IN | IOC_OUT)
#endif

#define LOOPHOLE_IOC(inout, group, num, len) \
   ((unsigned long)(inout) | (((unsigned long)(len) & IOCPARM_MASK) << 16) | ((group) << 8) | (num))

#ifndef _IO
#define _IO(g, n) LOOPHOLE_IOC(IOC_VOID, (g), (n), 0)
#endif
#ifndef _IOR
#define _IOR(g, n, t) LOOPHOLE_IOC(IOC_OUT, (g), (n), sizeof(t))
#endif
#ifndef _IOW
#define _IOW(g, n, t) LOOPHOLE_IOC(IOC_IN, (g), (n), sizeof(t))
#endif
#ifndef _IOWR
#define _IOWR(g, n, t) LOOPHOLE_IOC(IOC_INOUT, (g), (n), sizeof(t))
#endif

#endif
