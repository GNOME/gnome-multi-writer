/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>.
 * Copyright (C) 2014-2015 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
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
