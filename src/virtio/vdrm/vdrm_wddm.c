/*
 * Copyright © 2026 loophole contributors
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
/* NTSTATUS, which WIN32_LEAN_AND_MEAN leaves out but d3dkmthk.h needs. */
#include <winternl.h>

#include <d3dkmthk.h>

#include "viogpum.h"

#include "vdrm.h"

#include "drm-uapi/drm.h"
#include "drm-uapi/drm_fourcc.h"
#include "drm-uapi/virtgpu_drm.h"
#include "c11/threads.h"
#include "util/log.h"
#include "util/macros.h"
#include "util/os_misc.h"
#include "util/os_time.h"
#include "util/simple_mtx.h"
#include "util/u_dynarray.h"
#include "util/u_idalloc.h"
#include "util/u_math.h"
#include "util/u_sync_provider.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

#define WDDM_CAPSET_DRM 6
#define WDDM_SHMEM_SIZE 0x4000

struct wddm_bo {
   D3DKMT_HANDLE alloc;
   D3DKMT_HANDLE resource;
   uint32_t res_id;
   uint64_t size;
   void *map;
};

struct wddm_point {
   uint64_t value; /* timeline point, 0 for the payload of a binary syncobj */
   uint32_t ring;  /* 0: signaled by the CPU */
   uint64_t seq;   /* ring submission number, when ring != 0 */
};

struct wddm_syncobj {
   bool live;
   struct util_dynarray points;
};

struct wddm_sync_state {
   uint32_t refcount;
   mtx_t lock;
   struct util_dynarray objs; /* struct wddm_syncobj, handle - 1 */
   const VIOGPU_RING_FENCE_PAGE *fences;
};

struct wddm_kmt {
   PFND3DKMT_ENUMADAPTERS2 enumAdapters2;
   PFND3DKMT_CLOSEADAPTER closeAdapter;
   PFND3DKMT_QUERYADAPTERINFO queryAdapterInfo;
   PFND3DKMT_ESCAPE escape;
   PFND3DKMT_CREATEDEVICE createDevice;
   PFND3DKMT_DESTROYDEVICE destroyDevice;
   PFND3DKMT_CREATECONTEXT createContext;
   PFND3DKMT_DESTROYCONTEXT destroyContext;
   PFND3DKMT_RENDER render;
   PFND3DKMT_CREATEALLOCATION createAllocation;
   PFND3DKMT_DESTROYALLOCATION destroyAllocation;
};

struct wddm_device {
   struct vdrm_device base;
   struct wddm_kmt kmt;

   D3DKMT_HANDLE adapter;
   D3DKMT_HANDLE device;
   D3DKMT_HANDLE context;

   simple_mtx_t submit_lock;
   void *cmd_buf;
   UINT cmd_size;
   uint64_t submitted[VIOGPU_MAX_RINGS];

   /* Submissions use VIOGPU_SUBMIT_DIRECT escapes, or D3DKMTRender with
    * VDRM_WDDM_SUBMIT=render.
    */
   bool direct_submit;
   VIOGPU_ESCAPE *direct_buf;
   size_t direct_size;

   simple_mtx_t bo_lock;
   struct util_idalloc bo_ids;
   struct util_dynarray bos; /* struct wddm_bo, handle - 1 */
   struct wddm_bo shmem;

   struct wddm_sync_state *sync;
};
DEFINE_CAST(vdrm_device, wddm_device)

static int
set_errno(int err)
{
   errno = err;
   return -1;
}

static bool
load_kmt(struct wddm_kmt *k)
{
   HMODULE gdi = LoadLibraryA("gdi32.dll");
   if (!gdi)
      return false;
#define LOAD(field, sym)                                                                           \
   k->field = (void *)GetProcAddress(gdi, sym);                                                    \
   if (!k->field)                                                                                  \
      return false;
   LOAD(enumAdapters2, "D3DKMTEnumAdapters2");
   LOAD(closeAdapter, "D3DKMTCloseAdapter");
   LOAD(queryAdapterInfo, "D3DKMTQueryAdapterInfo");
   LOAD(escape, "D3DKMTEscape");
   LOAD(createDevice, "D3DKMTCreateDevice");
   LOAD(destroyDevice, "D3DKMTDestroyDevice");
   LOAD(createContext, "D3DKMTCreateContext");
   LOAD(destroyContext, "D3DKMTDestroyContext");
   LOAD(render, "D3DKMTRender");
   LOAD(createAllocation, "D3DKMTCreateAllocation");
   LOAD(destroyAllocation, "D3DKMTDestroyAllocation");
#undef LOAD
   return true;
}

static NTSTATUS
wddm_escape(struct wddm_device *wd, VIOGPU_ESCAPE *esc, bool with_device)
{
   D3DKMT_ESCAPE e = {
      .hAdapter = wd->adapter,
      .hDevice = with_device ? wd->device : 0,
      .Type = D3DKMT_ESCAPE_DRIVERPRIVATE,
      .pPrivateDriverData = esc,
      .PrivateDriverDataSize = sizeof(*esc),
   };
   return wd->kmt.escape(&e);
}

static bool
open_adapter(struct wddm_device *wd)
{
   D3DKMT_ENUMADAPTERS2 en = {0};
   if (!NT_SUCCESS(wd->kmt.enumAdapters2(&en)) || !en.NumAdapters)
      return false;
   D3DKMT_ADAPTERINFO *infos = calloc(en.NumAdapters, sizeof(*infos));
   if (!infos)
      return false;
   en.pAdapters = infos;
   if (!NT_SUCCESS(wd->kmt.enumAdapters2(&en))) {
      free(infos);
      return false;
   }
   for (ULONG i = 0; i < en.NumAdapters; i++) {
      VIOGPU_ADAPTERINFO vi = {0};
      D3DKMT_QUERYADAPTERINFO q = {
         .hAdapter = infos[i].hAdapter,
         .Type = KMTQAITYPE_UMDRIVERPRIVATE,
         .pPrivateDriverData = &vi,
         .PrivateDriverDataSize = sizeof(vi),
      };
      bool ours = !wd->adapter && NT_SUCCESS(wd->kmt.queryAdapterInfo(&q)) &&
                  vi.IamVioGPU == VIOGPU_IAM && vi.Flags.Supports3d && vi.Flags.HasShmem &&
                  (vi.SupportedCapsetIDs & (1ull << WDDM_CAPSET_DRM));
      if (ours) {
         wd->adapter = infos[i].hAdapter;
      } else {
         D3DKMT_CLOSEADAPTER c = {.hAdapter = infos[i].hAdapter};
         (void)wd->kmt.closeAdapter(&c);
      }
   }
   free(infos);
   return wd->adapter != 0;
}

static int
wddm_submit_direct(struct wddm_device *wd, uint32_t ring_idx, const void *cmd, uint32_t size)
{
   simple_mtx_assert_locked(&wd->submit_lock);
   if (size > VIOGPU_SUBMIT_DIRECT_MAX) {
      mesa_loge("vdrm_wddm: %u byte command exceeds the direct-submit limit", size);
      return -1;
   }
   const size_t need = sizeof(VIOGPU_ESCAPE) + size;
   if (need > wd->direct_size) {
      void *buf = realloc(wd->direct_buf, need);
      if (!buf)
         return -1;
      wd->direct_buf = buf;
      wd->direct_size = need;
   }
   memset(wd->direct_buf, 0, sizeof(VIOGPU_ESCAPE));
   wd->direct_buf->Type = VIOGPU_SUBMIT_DIRECT;
   wd->direct_buf->DataLength = sizeof(VIOGPU_SUBMIT_DIRECT_REQ);
   wd->direct_buf->SubmitDirect.RingIdx = ring_idx;
   wd->direct_buf->SubmitDirect.Size = size;
   memcpy(wd->direct_buf + 1, cmd, size);

   D3DKMT_ESCAPE e = {
      .hAdapter = wd->adapter,
      .hDevice = wd->device,
      .Type = D3DKMT_ESCAPE_DRIVERPRIVATE,
      .pPrivateDriverData = wd->direct_buf,
      .PrivateDriverDataSize = (UINT)need,
   };
   NTSTATUS st = wd->kmt.escape(&e);
   if (!NT_SUCCESS(st)) {
      mesa_loge("vdrm_wddm: SUBMIT_DIRECT failed: 0x%lx", st);
      return -1;
   }
   if (ring_idx)
      wd->submitted[ring_idx]++;
   return 0;
}

static int
wddm_render(struct wddm_device *wd, uint32_t ring_idx, const void *cmd, uint32_t size)
{
   simple_mtx_assert_locked(&wd->submit_lock);
   if (wd->direct_submit)
      return wddm_submit_direct(wd, ring_idx, cmd, size);
   if (size + sizeof(VIOGPU_COMMAND_HDR) > wd->cmd_size) {
      mesa_loge("vdrm_wddm: %u byte command exceeds the %u byte DMA buffer", size, wd->cmd_size);
      return -1;
   }
   VIOGPU_COMMAND_HDR *hdr = wd->cmd_buf;
   hdr->type = VIOGPU_CMD_SUBMIT;
   hdr->size = size;
   hdr->flags = VIOGPU_EXECBUF_RING_IDX;
   hdr->ring_idx = ring_idx;
   memcpy(hdr + 1, cmd, size);

   D3DKMT_RENDER r = {
      .hContext = wd->context,
      .CommandLength = (UINT)(sizeof(*hdr) + size),
   };
   NTSTATUS st = wd->kmt.render(&r);
   wd->cmd_buf = r.pNewCommandBuffer;
   wd->cmd_size = r.NewCommandBufferSize;
   if (!NT_SUCCESS(st)) {
      mesa_loge("vdrm_wddm: D3DKMTRender failed: 0x%lx", st);
      return -1;
   }
   if (ring_idx)
      wd->submitted[ring_idx]++;
   return 0;
}

static struct wddm_syncobj *
sync_lookup(struct wddm_sync_state *s, uint32_t handle)
{
   if (!handle || handle > util_dynarray_num_elements(&s->objs, struct wddm_syncobj))
      return NULL;
   struct wddm_syncobj *o = util_dynarray_element(&s->objs, struct wddm_syncobj, handle - 1);
   return o->live ? o : NULL;
}

static bool
point_signaled(const struct wddm_sync_state *s, const struct wddm_point *p)
{
   return p->ring == 0 || (s->fences && s->fences->Completed[p->ring] >= p->seq);
}

static uint64_t
sync_payload(const struct wddm_sync_state *s, const struct wddm_syncobj *o)
{
   uint64_t payload = 0;
   util_dynarray_foreach (&o->points, struct wddm_point, p) {
      if (!point_signaled(s, p))
         break;
      payload = p->value;
   }
   return payload;
}

static void
sync_prune(const struct wddm_sync_state *s, struct wddm_syncobj *o)
{
   unsigned n = util_dynarray_num_elements(&o->points, struct wddm_point);
   unsigned keep_from = 0;
   for (unsigned i = 0; i < n; i++) {
      if (!point_signaled(s, util_dynarray_element(&o->points, struct wddm_point, i)))
         break;
      keep_from = i;
   }
   if (keep_from) {
      struct wddm_point *pts = o->points.data;
      memmove(pts, pts + keep_from, (n - keep_from) * sizeof(*pts));
      o->points.size = (n - keep_from) * sizeof(*pts);
   }
}

static void
sync_add_point(struct wddm_sync_state *s, struct wddm_syncobj *o, struct wddm_point pt)
{
   if (pt.value == 0) {
      util_dynarray_clear(&o->points);
      util_dynarray_append(&o->points, pt);
   } else {
      unsigned n = util_dynarray_num_elements(&o->points, struct wddm_point);
      unsigned at = n;
      while (at > 0 && util_dynarray_element(&o->points, struct wddm_point, at - 1)->value > pt.value)
         at--;
      util_dynarray_append(&o->points, pt);
      struct wddm_point *pts = o->points.data;
      memmove(pts + at + 1, pts + at, (n - at) * sizeof(*pts));
      pts[at] = pt;
      sync_prune(s, o);
   }
}

enum wait_state { WAIT_NO_FENCE, WAIT_PENDING, WAIT_DONE };

static enum wait_state
sync_test(const struct wddm_sync_state *s, const struct wddm_syncobj *o, uint64_t value,
          bool timeline, bool available)
{
   if (!timeline) {
      if (!util_dynarray_num_elements(&o->points, struct wddm_point))
         return WAIT_NO_FENCE;
      const struct wddm_point *p = util_dynarray_element(&o->points, struct wddm_point, 0);
      return available || point_signaled(s, p) ? WAIT_DONE : WAIT_PENDING;
   }
   if (available) {
      util_dynarray_foreach (&o->points, struct wddm_point, p)
         if (p->value >= value)
            return WAIT_DONE;
      return WAIT_NO_FENCE;
   }
   if (sync_payload(s, o) >= value)
      return WAIT_DONE;
   util_dynarray_foreach (&o->points, struct wddm_point, p)
      if (p->value >= value)
         return WAIT_PENDING;
   return WAIT_NO_FENCE;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* Sleeps ~200 us. */
static void
short_sleep(void)
{
   static _Thread_local HANDLE timer;
   if (!timer)
      timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
   LARGE_INTEGER due = {.QuadPart = -2000}; /* relative, 100 ns units */
   if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
      WaitForSingleObject(timer, INFINITE);
   else
      SwitchToThread();
}

/* Waits on CPU or GPU progress. GPU completion is only visible through the
 * fence page.
 */
static int
sync_wait_common(struct wddm_sync_state *s, const uint32_t *handles, const uint64_t *values,
                 unsigned count, int64_t abs_timeout_ns, unsigned flags, uint32_t *first_signaled,
                 bool timeline)
{
   const bool wait_all = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
   const bool for_submit = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
   const bool available = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
   const int64_t start = os_time_get_nano();
   unsigned spins = 0;

   mtx_lock(&s->lock);
   for (;;) {
      unsigned done = 0, first = UINT32_MAX;
      for (unsigned i = 0; i < count; i++) {
         struct wddm_syncobj *o = sync_lookup(s, handles[i]);
         if (!o) {
            mtx_unlock(&s->lock);
            return set_errno(ENOENT);
         }
         enum wait_state st = sync_test(s, o, values ? values[i] : 0, timeline, available);
         if (st == WAIT_NO_FENCE && !for_submit) {
            mtx_unlock(&s->lock);
            return set_errno(EINVAL);
         }
         if (st == WAIT_DONE) {
            done++;
            if (first == UINT32_MAX)
               first = i;
         }
      }
      if (wait_all ? done == count : done > 0) {
         mtx_unlock(&s->lock);
         if (first_signaled)
            *first_signaled = first;
         return 0;
      }

      int64_t now = os_time_get_nano();
      if (now >= abs_timeout_ns) {
         mtx_unlock(&s->lock);
         return set_errno(ETIME);
      }
      mtx_unlock(&s->lock);
      if (now - start < 1000000) {
         if (++spins % 64)
            YieldProcessor();
         else
            SwitchToThread();
      } else {
         short_sleep();
      }
      mtx_lock(&s->lock);
   }
}

struct wddm_sync_provider {
   struct util_sync_provider base;
   struct wddm_sync_state *s;
};
DEFINE_CAST(util_sync_provider, wddm_sync_provider)

static struct wddm_sync_state *
state_of(struct util_sync_provider *p)
{
   return to_wddm_sync_provider(p)->s;
}

static int
wsync_create(struct util_sync_provider *p, uint32_t flags, uint32_t *handle)
{
   struct wddm_sync_state *s = state_of(p);
   mtx_lock(&s->lock);
   unsigned n = util_dynarray_num_elements(&s->objs, struct wddm_syncobj);
   unsigned idx = n;
   for (unsigned i = 0; i < n; i++) {
      if (!util_dynarray_element(&s->objs, struct wddm_syncobj, i)->live) {
         idx = i;
         break;
      }
   }
   if (idx == n)
      util_dynarray_append(&s->objs, (struct wddm_syncobj){0});
   struct wddm_syncobj *o = util_dynarray_element(&s->objs, struct wddm_syncobj, idx);
   o->live = true;
   util_dynarray_init(&o->points, NULL);
   if (flags & DRM_SYNCOBJ_CREATE_SIGNALED)
      util_dynarray_append(&o->points, (struct wddm_point){0});
   *handle = idx + 1;
   mtx_unlock(&s->lock);
   return 0;
}

static int
wsync_destroy(struct util_sync_provider *p, uint32_t handle)
{
   struct wddm_sync_state *s = state_of(p);
   mtx_lock(&s->lock);
   struct wddm_syncobj *o = sync_lookup(s, handle);
   if (o) {
      util_dynarray_fini(&o->points);
      o->live = false;
   }
   mtx_unlock(&s->lock);
   return o ? 0 : set_errno(ENOENT);
}

static int
wsync_unsupported_fd(struct util_sync_provider *p, uint32_t handle, int *fd)
{
   return set_errno(EOPNOTSUPP);
}

static int
wsync_unsupported_import(struct util_sync_provider *p, int fd, uint32_t *handle)
{
   return set_errno(EOPNOTSUPP);
}

static int
wsync_unsupported_sync_file(struct util_sync_provider *p, uint32_t handle, int fd)
{
   return set_errno(EOPNOTSUPP);
}

static int
wsync_wait(struct util_sync_provider *p, uint32_t *handles, unsigned num_handles,
           int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
   return sync_wait_common(state_of(p), handles, NULL, num_handles, timeout_nsec, flags,
                           first_signaled, false);
}

static int
wsync_timeline_wait(struct util_sync_provider *p, uint32_t *handles, uint64_t *points,
                    unsigned num_handles, int64_t timeout_nsec, unsigned flags,
                    uint32_t *first_signaled)
{
   return sync_wait_common(state_of(p), handles, points, num_handles, timeout_nsec, flags,
                           first_signaled, true);
}

static int
wsync_reset(struct util_sync_provider *p, const uint32_t *handles, uint32_t count)
{
   struct wddm_sync_state *s = state_of(p);
   mtx_lock(&s->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct wddm_syncobj *o = sync_lookup(s, handles[i]);
      if (o)
         util_dynarray_clear(&o->points);
   }
   mtx_unlock(&s->lock);
   return 0;
}

static int
wsync_signal(struct util_sync_provider *p, const uint32_t *handles, uint32_t count)
{
   struct wddm_sync_state *s = state_of(p);
   mtx_lock(&s->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct wddm_syncobj *o = sync_lookup(s, handles[i]);
      if (o)
         sync_add_point(s, o, (struct wddm_point){0});
   }
   mtx_unlock(&s->lock);
   return 0;
}

static int
wsync_timeline_signal(struct util_sync_provider *p, const uint32_t *handles, uint64_t *points,
                      uint32_t count)
{
   struct wddm_sync_state *s = state_of(p);
   mtx_lock(&s->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct wddm_syncobj *o = sync_lookup(s, handles[i]);
      if (o)
         sync_add_point(s, o, (struct wddm_point){.value = points[i]});
   }
   mtx_unlock(&s->lock);
   return 0;
}

static int
wsync_query(struct util_sync_provider *p, uint32_t *handles, uint64_t *points, uint32_t count,
            uint32_t flags)
{
   struct wddm_sync_state *s = state_of(p);
   mtx_lock(&s->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct wddm_syncobj *o = sync_lookup(s, handles[i]);
      if (!o) {
         mtx_unlock(&s->lock);
         return set_errno(ENOENT);
      }
      if (flags & DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) {
         const struct wddm_point *last = util_dynarray_last_ptr(&o->points, struct wddm_point);
         points[i] = last ? last->value : 0;
      } else {
         points[i] = sync_payload(s, o);
      }
   }
   mtx_unlock(&s->lock);
   return 0;
}

static int
wsync_transfer(struct util_sync_provider *p, uint32_t dst_handle, uint64_t dst_point,
               uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
   struct wddm_sync_state *s = state_of(p);
   mtx_lock(&s->lock);
   struct wddm_syncobj *dst = sync_lookup(s, dst_handle), *src = sync_lookup(s, src_handle);
   if (!dst || !src) {
      mtx_unlock(&s->lock);
      return set_errno(ENOENT);
   }
   const struct wddm_point signaled = {0};
   const struct wddm_point *from = NULL;
   util_dynarray_foreach (&src->points, struct wddm_point, pt) {
      if (pt->value >= src_point) {
         from = pt;
         break;
      }
   }
   if (!from && src_point && sync_payload(s, src) >= src_point)
      from = &signaled;
   if (!from) {
      mtx_unlock(&s->lock);
      return set_errno(EINVAL);
   }
   struct wddm_point pt = *from;
   pt.value = dst_point;
   sync_add_point(s, dst, pt);
   mtx_unlock(&s->lock);
   return 0;
}

static void
sync_state_unref(struct wddm_sync_state *s)
{
   mtx_lock(&s->lock);
   bool last = --s->refcount == 0;
   mtx_unlock(&s->lock);
   if (!last)
      return;
   util_dynarray_foreach (&s->objs, struct wddm_syncobj, o)
      if (o->live)
         util_dynarray_fini(&o->points);
   util_dynarray_fini(&s->objs);
   mtx_destroy(&s->lock);
   free(s);
}

static void wsync_finalize(struct util_sync_provider *p);
static struct util_sync_provider *wsync_clone(struct util_sync_provider *p);

static struct util_sync_provider *
sync_provider_new(struct wddm_sync_state *s)
{
   struct wddm_sync_provider *wp = calloc(1, sizeof(*wp));
   if (!wp)
      return NULL;
   mtx_lock(&s->lock);
   s->refcount++;
   mtx_unlock(&s->lock);
   wp->s = s;
   wp->base = (struct util_sync_provider){
      .create = wsync_create,
      .destroy = wsync_destroy,
      .handle_to_fd = wsync_unsupported_fd,
      .fd_to_handle = wsync_unsupported_import,
      .import_sync_file = wsync_unsupported_sync_file,
      .export_sync_file = wsync_unsupported_fd,
      .wait = wsync_wait,
      .reset = wsync_reset,
      .signal = wsync_signal,
      .timeline_signal = wsync_timeline_signal,
      .timeline_wait = wsync_timeline_wait,
      .query = wsync_query,
      .transfer = wsync_transfer,
      .finalize = wsync_finalize,
      .clone = wsync_clone,
   };
   return &wp->base;
}

static void
wsync_finalize(struct util_sync_provider *p)
{
   sync_state_unref(state_of(p));
   free(to_wddm_sync_provider(p));
}

static struct util_sync_provider *
wsync_clone(struct util_sync_provider *p)
{
   return sync_provider_new(state_of(p));
}

static void
sync_attach_submit(struct wddm_sync_state *s, const struct drm_virtgpu_execbuffer_syncobj *outs,
                   uint32_t count, uint32_t ring, uint64_t seq)
{
   mtx_lock(&s->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct wddm_syncobj *o = sync_lookup(s, outs[i].handle);
      if (o)
         sync_add_point(s, o, (struct wddm_point){.value = outs[i].point, .ring = ring, .seq = seq});
   }
   mtx_unlock(&s->lock);
}

/* True if a submission to `ring` cannot run before the fences a wait for
 * `value` depends on: they are signaled already or were submitted earlier
 * to the same ring, which the host and the GPU execute in order. Called
 * with the sync state locked.
 */
static bool
sync_ordered_before(const struct wddm_sync_state *s, const struct wddm_syncobj *o, uint64_t value,
                    uint32_t ring)
{
   bool any = false;
   util_dynarray_foreach (&o->points, struct wddm_point, p) {
      if (!point_signaled(s, p) && p->ring != ring)
         return false;
      any = true;
      if (p->value >= value)
         return true;
   }
   return any && sync_payload(s, o) >= value;
}

static int
wddm_wait_syncobjs(struct wddm_device *wd, const struct drm_virtgpu_execbuffer_syncobj *ins,
                   uint32_t count, uint32_t ring)
{
   if (!count)
      return 0;
   uint32_t *handles = malloc(count * sizeof(*handles));
   uint64_t *points = malloc(count * sizeof(*points));
   if (!handles || !points) {
      free(handles);
      free(points);
      return -ENOMEM;
   }
   bool timeline = false;
   uint32_t waits = 0;
   mtx_lock(&wd->sync->lock);
   for (uint32_t i = 0; i < count; i++) {
      struct wddm_syncobj *o = sync_lookup(wd->sync, ins[i].handle);
      if (ring && o && sync_ordered_before(wd->sync, o, ins[i].point, ring))
         continue;
      handles[waits] = ins[i].handle;
      points[waits] = ins[i].point;
      timeline |= ins[i].point != 0;
      waits++;
   }
   mtx_unlock(&wd->sync->lock);
   count = waits;
   if (!count) {
      free(handles);
      free(points);
      return 0;
   }
   const unsigned flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
   int r = sync_wait_common(wd->sync, handles, timeline ? points : NULL, count, INT64_MAX, flags,
                            NULL, timeline);
   free(handles);
   free(points);
   return r ? -errno : 0;
}

static int
wddm_execbuf_locked(struct vdrm_device *vdev, struct vdrm_execbuf_params *p, void *command,
                    unsigned size)
{
   struct wddm_device *wd = to_wddm_device(vdev);

   int r = wddm_wait_syncobjs(wd, p->in_syncobjs, p->num_in_syncobjs, p->ring_idx);
   if (r)
      return r;

   simple_mtx_lock(&wd->submit_lock);
   r = wddm_render(wd, p->ring_idx, command, size);
   uint64_t seq = wd->submitted[p->ring_idx];
   simple_mtx_unlock(&wd->submit_lock);
   if (r)
      return r;

   if (p->ring_idx == 0) {
      vdrm_host_sync(vdev, p->req);
      sync_attach_submit(wd->sync, p->out_syncobjs, p->num_out_syncobjs, 0, 0);
   } else {
      sync_attach_submit(wd->sync, p->out_syncobjs, p->num_out_syncobjs, p->ring_idx, seq);
   }

   if (p->needs_out_fence_fd) {
      while (p->ring_idx && wd->sync->fences && wd->sync->fences->Completed[p->ring_idx] < seq)
         thrd_yield();
      p->fence_fd = -1;
   }
   return 0;
}

static int
wddm_flush_locked(struct vdrm_device *vdev, uintptr_t *fencep)
{
   struct wddm_device *wd = to_wddm_device(vdev);
   if (fencep)
      *fencep = 0;
   if (!vdev->reqbuf_len)
      return 0;
   simple_mtx_lock(&wd->submit_lock);
   int r = wddm_render(wd, 0, vdev->reqbuf, vdev->reqbuf_len);
   simple_mtx_unlock(&wd->submit_lock);
   vdev->reqbuf_len = 0;
   vdev->reqbuf_cnt = 0;
   return r;
}

static void
wddm_wait_fence(struct vdrm_device *vdev, uintptr_t fence)
{
}

static uint32_t
wddm_dmabuf_to_handle(struct vdrm_device *vdev, int fd)
{
   return 0;
}

static struct wddm_bo *
bo_lookup(struct wddm_device *wd, uint32_t handle)
{
   if (!handle || handle > util_dynarray_num_elements(&wd->bos, struct wddm_bo))
      return NULL;
   return util_dynarray_element(&wd->bos, struct wddm_bo, handle - 1);
}

static uint32_t
wddm_handle_to_res_id(struct vdrm_device *vdev, uint32_t handle)
{
   struct wddm_device *wd = to_wddm_device(vdev);
   simple_mtx_lock(&wd->bo_lock);
   struct wddm_bo *bo = bo_lookup(wd, handle);
   uint32_t res_id = bo ? bo->res_id : 0;
   simple_mtx_unlock(&wd->bo_lock);
   return res_id;
}

static bool
create_blob(struct wddm_device *wd, size_t size, uint32_t blob_flags, uint64_t blob_id,
            struct wddm_bo *bo)
{
   memset(bo, 0, sizeof(*bo));
   bo->size = align64(size, 4096);
   VIOGPU_CREATE_ALLOCATION_EXCHANGE priv = {
      .Type = VIOGPU_RESOURCE_TYPE_BLOB,
      .OptionsBlob =
         {
            .blob_mem = VIOGPU_BLOB_MEM_HOST3D,
            .blob_flags = blob_flags & (VIOGPU_BLOB_FLAG_USE_MAPPABLE | VIOGPU_BLOB_FLAG_USE_SHAREABLE),
            .blob_id = blob_id,
         },
      .Size = bo->size,
   };
   VIOGPU_CREATE_RESOURCE_EXCHANGE res_priv = {0};
   D3DDDI_ALLOCATIONINFO ai = {.pPrivateDriverData = &priv, .PrivateDriverDataSize = sizeof(priv)};
   D3DKMT_CREATEALLOCATION ca = {
      .hDevice = wd->device,
      .pPrivateDriverData = &res_priv,
      .PrivateDriverDataSize = sizeof(res_priv),
      .NumAllocations = 1,
      .pAllocationInfo = &ai,
      .Flags = {.CreateResource = 1},
   };
   NTSTATUS st = wd->kmt.createAllocation(&ca);
   if (!NT_SUCCESS(st)) {
      mesa_loge("vdrm_wddm: CreateAllocation(blob %" PRIu64 ") failed: 0x%lx", blob_id, st);
      return false;
   }
   bo->alloc = ai.hAllocation;
   bo->resource = ca.hResource;

   VIOGPU_ESCAPE esc = {
      .Type = VIOGPU_RES_INFO,
      .DataLength = sizeof(VIOGPU_RES_INFO_REQ),
      .ResourceInfo = {.ResHandle = bo->alloc},
   };
   /* The KMD issues RESOURCE_CREATE_BLOB asynchronously. RES_INFO reports
    * IsCreated once it is queued, and maps mappable blobs synchronously.
    */
   const int64_t deadline = os_time_get_nano() + 2000000000ll;
   for (unsigned tries = 0;; tries++) {
      st = wddm_escape(wd, &esc, true);
      if (!NT_SUCCESS(st) || esc.ResourceInfo.IsCreated || os_time_get_nano() > deadline)
         break;
      Sleep(tries < 16 ? 0 : 1);
   }
   if (!NT_SUCCESS(st) || !esc.ResourceInfo.IsBlob || !esc.ResourceInfo.IsCreated) {
      mesa_loge("vdrm_wddm: RES_INFO failed: 0x%lx (blob %d, created %d)", st,
                esc.ResourceInfo.IsBlob, esc.ResourceInfo.IsCreated);
      D3DKMT_DESTROYALLOCATION d = {.hDevice = wd->device, .hResource = bo->resource};
      (void)wd->kmt.destroyAllocation(&d);
      return false;
   }
   bo->res_id = esc.ResourceInfo.Id;
   bo->map = (void *)(uintptr_t)esc.ResourceInfo.UserVa;
   return true;
}

static uint32_t
wddm_bo_create(struct vdrm_device *vdev, size_t size, uint32_t blob_flags, uint64_t blob_id,
               uint32_t blob_hints, struct vdrm_ccmd_req *req)
{
   struct wddm_device *wd = to_wddm_device(vdev);

   /* The host must have run the allocation request (e.g. amdgpu GEM_NEW)
    * before the KMD creates the blob that refers to it by blob_id.
    */
   if (req) {
      simple_mtx_lock(&wd->submit_lock);
      int r = wddm_render(wd, 0, req, req->len);
      simple_mtx_unlock(&wd->submit_lock);
      if (r)
         return 0;
      vdrm_host_sync(vdev, req);
   }

   struct wddm_bo bo;
   if (!create_blob(wd, size, blob_flags, blob_id, &bo))
      return 0;

   simple_mtx_lock(&wd->bo_lock);
   uint32_t idx = util_idalloc_alloc(&wd->bo_ids);
   if (idx >= util_dynarray_num_elements(&wd->bos, struct wddm_bo))
      util_dynarray_resize(&wd->bos, struct wddm_bo, idx + 1);
   *util_dynarray_element(&wd->bos, struct wddm_bo, idx) = bo;
   simple_mtx_unlock(&wd->bo_lock);
   return idx + 1;
}

static int
wddm_bo_wait(struct vdrm_device *vdev, uint32_t handle)
{
   return 0;
}

static void *
wddm_bo_map(struct vdrm_device *vdev, uint32_t handle, size_t size, void *placed_addr)
{
   struct wddm_device *wd = to_wddm_device(vdev);
   simple_mtx_lock(&wd->bo_lock);
   struct wddm_bo *bo = bo_lookup(wd, handle);
   void *map = bo ? bo->map : NULL;
   simple_mtx_unlock(&wd->bo_lock);
   if (placed_addr && map != placed_addr)
      return NULL;
   return map;
}

static void
wddm_bo_unmap(struct vdrm_device *vdev, uint32_t handle, void *addr, size_t size)
{
}

static int
wddm_bo_export_dmabuf(struct vdrm_device *vdev, uint32_t handle)
{
   return -1;
}

/* virtio-gpu formats are named by their byte order, DRM's by the order in a
 * little-endian word.
 */
static uint32_t
virtio_gpu_format(uint32_t drm_format)
{
   switch (drm_format) {
   case DRM_FORMAT_XRGB8888:
      return 2; /* VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM */
   case DRM_FORMAT_ARGB8888:
      return 1; /* VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM */
   case DRM_FORMAT_XBGR8888:
      return 134; /* VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM */
   case DRM_FORMAT_ABGR8888:
      return 67; /* VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM */
   default:
      return 0;
   }
}

static int
wddm_bo_scanout(struct vdrm_device *vdev, uint32_t handle, uint32_t width, uint32_t height,
                uint32_t stride, uint32_t drm_format)
{
   struct wddm_device *wd = to_wddm_device(vdev);
   VIOGPU_ESCAPE esc = {
      .Type = VIOGPU_SCANOUT_BLOB,
      .DataLength = sizeof(VIOGPU_SCANOUT_BLOB_REQ),
   };

   if (handle) {
      simple_mtx_lock(&wd->bo_lock);
      struct wddm_bo *bo = bo_lookup(wd, handle);
      esc.ScanoutBlob.ResHandle = bo ? bo->alloc : 0;
      simple_mtx_unlock(&wd->bo_lock);
      esc.ScanoutBlob.Format = virtio_gpu_format(drm_format);
      if (!esc.ScanoutBlob.ResHandle || !esc.ScanoutBlob.Format)
         return -EINVAL;
      esc.ScanoutBlob.Width = width;
      esc.ScanoutBlob.Height = height;
      esc.ScanoutBlob.Stride = stride;
   }

   NTSTATUS st = wddm_escape(wd, &esc, true);
   if (!NT_SUCCESS(st)) {
      mesa_loge("vdrm_wddm: SCANOUT_BLOB failed: 0x%lx", st);
      return -EINVAL;
   }
   return 0;
}

static void
destroy_blob(struct wddm_device *wd, struct wddm_bo *bo)
{
   if (!bo->alloc)
      return;
   D3DKMT_DESTROYALLOCATION d = {
      .hDevice = wd->device,
      .hResource = bo->resource,
      .phAllocationList = bo->resource ? NULL : &bo->alloc,
      .AllocationCount = bo->resource ? 0 : 1,
   };
   (void)wd->kmt.destroyAllocation(&d);
   memset(bo, 0, sizeof(*bo));
}

static void
wddm_bo_close(struct vdrm_device *vdev, uint32_t handle)
{
   struct wddm_device *wd = to_wddm_device(vdev);
   simple_mtx_lock(&wd->bo_lock);
   struct wddm_bo *bo = bo_lookup(wd, handle);
   if (bo) {
      destroy_blob(wd, bo);
      util_idalloc_free(&wd->bo_ids, handle - 1);
   }
   simple_mtx_unlock(&wd->bo_lock);
}

static void
wddm_close(struct vdrm_device *vdev)
{
   struct wddm_device *wd = to_wddm_device(vdev);
   util_dynarray_foreach (&wd->bos, struct wddm_bo, bo)
      destroy_blob(wd, bo);
   util_dynarray_fini(&wd->bos);
   util_idalloc_fini(&wd->bo_ids);
   destroy_blob(wd, &wd->shmem);
   free(wd->direct_buf);
   /* The fence page is unmapped with the device. Providers may outlive it
    * and must stop reading it first.
    */
   if (wd->sync) {
      mtx_lock(&wd->sync->lock);
      wd->sync->fences = NULL;
      mtx_unlock(&wd->sync->lock);
   }
   if (wd->context) {
      D3DKMT_DESTROYCONTEXT d = {.hContext = wd->context};
      (void)wd->kmt.destroyContext(&d);
   }
   if (wd->device) {
      D3DKMT_DESTROYDEVICE d = {.hDevice = wd->device};
      (void)wd->kmt.destroyDevice(&d);
   }
   if (wd->adapter) {
      D3DKMT_CLOSEADAPTER d = {.hAdapter = wd->adapter};
      (void)wd->kmt.closeAdapter(&d);
   }
   if (wd->sync)
      sync_state_unref(wd->sync);
   simple_mtx_destroy(&wd->bo_lock);
   simple_mtx_destroy(&wd->submit_lock);
}

static const struct vdrm_device_funcs funcs = {
   .execbuf_locked = wddm_execbuf_locked,
   .flush_locked = wddm_flush_locked,
   .wait_fence = wddm_wait_fence,
   .dmabuf_to_handle = wddm_dmabuf_to_handle,
   .handle_to_res_id = wddm_handle_to_res_id,
   .bo_create = wddm_bo_create,
   .bo_wait = wddm_bo_wait,
   .bo_map = wddm_bo_map,
   .bo_unmap = wddm_bo_unmap,
   .bo_export_dmabuf = wddm_bo_export_dmabuf,
   .bo_close = wddm_bo_close,
   .bo_scanout = wddm_bo_scanout,
   .close = wddm_close,
};

struct util_sync_provider *
vdrm_wddm_get_sync(struct vdrm_device *vdev)
{
   if (vdev->funcs != &funcs)
      return NULL;
   return sync_provider_new(to_wddm_device(vdev)->sync);
}

struct vdrm_device *
vdrm_wddm_connect(uint32_t context_type)
{
   struct wddm_device *wd = calloc(1, sizeof(*wd));
   if (!wd)
      return NULL;
   wd->base.funcs = &funcs;
   simple_mtx_init(&wd->submit_lock, mtx_plain);
   simple_mtx_init(&wd->bo_lock, mtx_plain);
   util_idalloc_init(&wd->bo_ids, 64);
   util_dynarray_init(&wd->bos, NULL);

   if (!load_kmt(&wd->kmt) || !open_adapter(wd))
      goto fail;

   VIOGPU_ESCAPE caps = {
      .Type = VIOGPU_GET_CAPS,
      .DataLength = sizeof(VIOGPU_CAPSET_REQ),
      .Capset = {.CapsetId = WDDM_CAPSET_DRM, .Size = sizeof(wd->base.caps),
                 .Capset = VioGpuUmPtr(&wd->base.caps)},
   };
   if (!NT_SUCCESS(wddm_escape(wd, &caps, false)) || wd->base.caps.context_type != context_type)
      goto fail;

   D3DKMT_CREATEDEVICE cd = {.hAdapter = wd->adapter};
   if (!NT_SUCCESS(wd->kmt.createDevice(&cd)))
      goto fail;
   wd->device = cd.hDevice;

   VIOGPU_ESCAPE init = {
      .Type = VIOGPU_CTX_INIT,
      .DataLength = sizeof(VIOGPU_CTX_INIT_REQ),
      .CtxInit = {.CapsetID = WDDM_CAPSET_DRM, .NumRings = VIOGPU_MAX_RINGS},
   };
   memcpy(init.CtxInit.DebugName, "mesa-vdrm", sizeof("mesa-vdrm"));
   if (!NT_SUCCESS(wddm_escape(wd, &init, true)))
      goto fail;

   const char *submit = os_get_option("VDRM_WDDM_SUBMIT");
   wd->direct_submit = !submit || strcmp(submit, "render") != 0;

   D3DKMT_CREATECONTEXT cc = {.hDevice = wd->device, .ClientHint = D3DKMT_CLIENTHINT_VULKAN};
   if (!NT_SUCCESS(wd->kmt.createContext(&cc)))
      goto fail;
   wd->context = cc.hContext;
   wd->cmd_buf = cc.pCommandBuffer;
   wd->cmd_size = cc.CommandBufferSize;

   /* blob_id 0 asks the host for the context's control/response page. */
   if (!create_blob(wd, WDDM_SHMEM_SIZE, VIOGPU_BLOB_FLAG_USE_MAPPABLE, 0, &wd->shmem) ||
       !wd->shmem.map)
      goto fail;
   wd->base.shmem = wd->shmem.map;
   uint32_t off = wd->base.shmem->rsp_mem_offset;
   if (off < sizeof(struct vdrm_shmem) || off >= WDDM_SHMEM_SIZE)
      goto fail;
   wd->base.rsp_mem = (uint8_t *)wd->base.shmem + off;
   wd->base.rsp_mem_len = WDDM_SHMEM_SIZE - off;

   VIOGPU_ESCAPE fe = {.Type = VIOGPU_RING_FENCES, .DataLength = sizeof(VIOGPU_RING_FENCES_REQ)};
   if (!NT_SUCCESS(wddm_escape(wd, &fe, true)) || !fe.RingFences.Page) {
      mesa_loge("vdrm_wddm: the viogpu KMD does not provide ring fences");
      goto fail;
   }

   wd->sync = calloc(1, sizeof(*wd->sync));
   if (!wd->sync)
      goto fail;
   wd->sync->refcount = 1;
   mtx_init(&wd->sync->lock, mtx_plain);
   util_dynarray_init(&wd->sync->objs, NULL);
   wd->sync->fences = (const VIOGPU_RING_FENCE_PAGE *)(uintptr_t)fe.RingFences.Page;
   return &wd->base;

fail:
   wddm_close(&wd->base);
   free(wd);
   return NULL;
}
