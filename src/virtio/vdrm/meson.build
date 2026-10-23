# Copyright 2023 Google LLC
# SPDX-License-Identifier: MIT

inc_virtio_vdrm = include_directories('.')

libvdrm_c_args = [no_override_init_args, soong_compat_c_args]
if with_amdgpu_virtio
  libvdrm_c_args += ['-DENABLE_DRM_AMDGPU']
endif
libvdrm_files = files('vdrm.c', 'vdrm.h')
if with_platform_windows
  libvdrm_files += files('vdrm_wddm.c')
else
  libvdrm_files += files('vdrm_virtgpu.c', 'vdrm_vpipe.c')
endif

libvdrm = static_library(
  'libvdrm',
  libvdrm_files,
  include_directories : [
    inc_include,
    inc_src,
    inc_virtio,
    inc_virtio_gpu,
  ],
  c_args : libvdrm_c_args,
  gnu_symbol_visibility: 'hidden',
  dependencies: [
    dep_libdrm,
    dep_valgrind,
    idep_mesautil,
  ],
  link_with: [
    _libmesa_util,
  ],
  build_by_default: false,
)
