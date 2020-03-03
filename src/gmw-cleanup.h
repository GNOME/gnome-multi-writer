/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>.
 * Copyright (C) 2014-2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#pragma once

#include <gio/gio.h>

#define GS_DEFINE_CLEANUP_FUNCTION0(Type, name, func) \
  static inline void name (void *v) \
  { \
    if (*(Type*)v) \
      func (*(Type*)v); \
  }

GS_DEFINE_CLEANUP_FUNCTION0(GObject*, gs_local_obj_unref, g_object_unref)

#define _cleanup_object_unref_ __attribute__ ((cleanup(gs_local_obj_unref)))
