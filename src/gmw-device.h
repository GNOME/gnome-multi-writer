/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
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

#ifndef __GMW_DEVICE_H__
#define __GMW_DEVICE_H__

#include <gio/gio.h>
#include <glib-object.h>
#include <gusb.h>
#include <udisks/udisks.h>

typedef struct _GmwDevice	GmwDevice;
typedef struct _GmwDeviceClass	GmwDeviceClass;

#define GMW_TYPE_DEVICE		(gmw_device_get_type ())
#define GMW_DEVICE(o)		(G_TYPE_CHECK_INSTANCE_CAST ((o), GMW_TYPE_DEVICE, GmwDevice))
#define GMW_IS_DEVICE(o)	(G_TYPE_CHECK_INSTANCE_TYPE ((o), GMW_TYPE_DEVICE))

struct _GmwDevice {
	GObject		 parent_instance;
};

struct _GmwDeviceClass {
	GObjectClass	 parent_class;
};

typedef enum {
	GMW_DEVICE_STATE_UNKNOWN,
	GMW_DEVICE_STATE_IDLE,
	GMW_DEVICE_STATE_WAITING,
	GMW_DEVICE_STATE_WRITE,
	GMW_DEVICE_STATE_VERIFY,
	GMW_DEVICE_STATE_SUCCESS,
	GMW_DEVICE_STATE_FAILED,
	GMW_DEVICE_STATE_LAST
} GmwDeviceState;

GType		 gmw_device_get_type		(void);
GmwDevice	*gmw_device_new			(void);
const gchar	*gmw_device_get_icon		(GmwDevice	*device);
gchar		*gmw_device_get_description	(GmwDevice	*device);
GmwDeviceState	 gmw_device_get_state		(GmwDevice	*device);
UDisksBlock	*gmw_device_get_udisks_block	(GmwDevice	*device);
const gchar	*gmw_device_get_name		(GmwDevice	*device);
const gchar	*gmw_device_get_block_path	(GmwDevice	*device);
const gchar	*gmw_device_get_hub_label	(GmwDevice	*device);
const gchar	*gmw_device_get_hub_id		(GmwDevice	*device);
guint8		 gmw_device_get_hub_root	(GmwDevice	*device);
const gchar	*gmw_device_get_object_path	(GmwDevice	*device);
const gchar	*gmw_device_get_order_display	(GmwDevice	*device);
const gchar	*gmw_device_get_order_process	(GmwDevice	*device);
const gchar	*gmw_device_get_sysfs_path	(GmwDevice	*device);
gdouble		 gmw_device_get_complete	(GmwDevice	*device);
gdouble		 gmw_device_get_speed_write	(GmwDevice	*device);
gdouble		 gmw_device_get_speed_read	(GmwDevice	*device);
guint64		 gmw_device_get_size		(GmwDevice	*device);

void		 gmw_device_set_state		(GmwDevice	*device,
						 GmwDeviceState	 device_state);
void		 gmw_device_set_udisks_block	(GmwDevice	*device,
						 UDisksBlock	*udisks_block);
void		 gmw_device_set_udisks_drive	(GmwDevice	*device,
						 UDisksDrive	*udisks_drive);
void		 gmw_device_set_usb_device	(GmwDevice	*device,
						 GUsbDevice	*usb_device);
void		 gmw_device_set_name		(GmwDevice	*device,
						 const gchar	*name);
void		 gmw_device_set_block_path	(GmwDevice	*device,
						 const gchar	*block_path);
void		 gmw_device_set_hub_id		(GmwDevice	*device,
						 const gchar	*hub_id);
void		 gmw_device_set_object_path	(GmwDevice	*device,
						 const gchar	*object_path);
void		 gmw_device_set_order_display	(GmwDevice	*device,
						 const gchar	*order_display);
void		 gmw_device_set_order_process	(GmwDevice	*device,
						 const gchar	*order_process);
void		 gmw_device_set_complete_read	(GmwDevice	*device,
						 gdouble	 complete);
void		 gmw_device_set_complete_write	(GmwDevice	*device,
						 gdouble	 complete);
void		 gmw_device_set_speed_write	(GmwDevice	*device,
						 gdouble	 speed_write);
void		 gmw_device_set_speed_read	(GmwDevice	*device,
						 gdouble	 speed_read);
void		 gmw_device_set_write_alloc	(GmwDevice	*device,
						 gdouble	 write_alloc);
void		 gmw_device_set_error		(GmwDevice	*device,
						 const GError	*error);

G_END_DECLS

#endif /* __GMW_DEVICE_H__ */

