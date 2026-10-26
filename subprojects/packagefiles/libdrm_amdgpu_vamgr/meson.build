# Copyright 2026 loophole contributors
# SPDX-License-Identifier: MIT

# Of libdrm, which doesn't build on Windows, only libdrm_amdgpu's VA manager: the amdgpu winsys
# allocates GPU addresses with it.
project('libdrm_amdgpu_vamgr', 'c', version : '2.4.134')

inc_vamgr = include_directories('amdgpu', 'include/drm')

libdrm_amdgpu_vamgr = static_library(
  'drm_amdgpu_vamgr',
  'amdgpu/amdgpu_vamgr.c',
  include_directories : [include_directories('windows', '.'), inc_vamgr],
  c_args : ['-DHAVE_LIBDRM_ATOMIC_PRIMITIVES=1', '-D_FILE_OFFSET_BITS=64'],
)

meson.override_dependency(
  'libdrm_amdgpu_vamgr',
  declare_dependency(link_with : libdrm_amdgpu_vamgr, include_directories : inc_vamgr),
)
