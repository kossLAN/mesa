# Copyright 2026 loophole contributors
# SPDX-License-Identifier: MIT

# TRE's POSIX regex for MinGW builds, which have none of their own. config.h and tre-config.h are
# what TRE's configure writes for MinGW.
project('tre', 'c', version : '0.9.0')

libtre = static_library(
  'tre',
  files(
    'lib/regcomp.c',
    'lib/regerror.c',
    'lib/regexec.c',
    'lib/tre-ast.c',
    'lib/tre-compile.c',
    'lib/tre-match-approx.c',
    'lib/tre-match-backtrack.c',
    'lib/tre-match-parallel.c',
    'lib/tre-mem.c',
    'lib/tre-parse.c',
    'lib/tre-stack.c',
  ),
  include_directories : include_directories('.', 'lib'),
  c_args : ['-DHAVE_CONFIG_H'],
)

meson.override_dependency(
  'tre',
  declare_dependency(
    link_with : libtre,
    compile_args : ['-DUSE_LOCAL_TRE_H'],
    include_directories : include_directories('local_includes'),
  ),
)
