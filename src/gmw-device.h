/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2014-2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#pragma once

#include <gio/gio.h>
#include <glib-object.h>
#include <gusb.h>
#include <udisks/udisks.h>

#define GMW_TYPE_DEVICE (gmw_device_get_type ())
G_DECLARE_FINAL_TYPE (GmwDevice, gmw_device, GMW, DEVICE, GObject)

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

GmwDevice	*gmw_device_new			(void);
const gchar	*gmw_device_get_icon		(GmwDevice	*self);
gchar		*gmw_device_get_description	(GmwDevice	*self);
gchar		*gmw_device_get_quirk_string	(GmwDevice	*self);
GmwDeviceState	 gmw_device_get_state		(GmwDevice	*self);
UDisksBlock	*gmw_device_get_udisks_block	(GmwDevice	*self);
const gchar	*gmw_device_get_name		(GmwDevice	*self);
const gchar	*gmw_device_get_block_path	(GmwDevice	*self);
const gchar	*gmw_device_get_hub_label	(GmwDevice	*self);
const gchar	*gmw_device_get_hub_id		(GmwDevice	*self);
guint8		 gmw_device_get_hub_root	(GmwDevice	*self);
const gchar	*gmw_device_get_object_path	(GmwDevice	*self);
const gchar	*gmw_device_get_order_display	(GmwDevice	*self);
const gchar	*gmw_device_get_order_process	(GmwDevice	*self);
const gchar	*gmw_device_get_sysfs_path	(GmwDevice	*self);
gdouble		 gmw_device_get_complete	(GmwDevice	*self);
gdouble		 gmw_device_get_speed_write	(GmwDevice	*self);
gdouble		 gmw_device_get_speed_read	(GmwDevice	*self);
guint64		 gmw_device_get_size		(GmwDevice	*self);

void		 gmw_device_set_state		(GmwDevice	*self,
						 GmwDeviceState	 device_state);
void		 gmw_device_set_udisks_block	(GmwDevice	*self,
						 UDisksBlock	*udisks_block);
void		 gmw_device_set_udisks_drive	(GmwDevice	*self,
						 UDisksDrive	*udisks_drive);
void		 gmw_device_set_usb_device	(GmwDevice	*self,
						 GUsbDevice	*usb_device);
void		 gmw_device_set_name		(GmwDevice	*self,
						 const gchar	*name);
void		 gmw_device_set_block_path	(GmwDevice	*self,
						 const gchar	*block_path);
void		 gmw_device_set_hub_id		(GmwDevice	*self,
						 const gchar	*hub_id);
void		 gmw_device_set_hub_label	(GmwDevice	*self,
						 const gchar	*hub_label);
void		 gmw_device_set_object_path	(GmwDevice	*self,
						 const gchar	*object_path);
void		 gmw_device_set_order_display	(GmwDevice	*self,
						 const gchar	*order_display);
void		 gmw_device_set_order_process	(GmwDevice	*self,
						 const gchar	*order_process);
void		 gmw_device_set_complete_read	(GmwDevice	*self,
						 gdouble	 complete);
void		 gmw_device_set_complete_write	(GmwDevice	*self,
						 gdouble	 complete);
void		 gmw_device_set_speed_write	(GmwDevice	*self,
						 gdouble	 speed_write);
void		 gmw_device_set_speed_read	(GmwDevice	*self,
						 gdouble	 speed_read);
void		 gmw_device_set_write_alloc	(GmwDevice	*self,
						 gdouble	 write_alloc);
void		 gmw_device_set_error		(GmwDevice	*self,
						 const GError	*error);
