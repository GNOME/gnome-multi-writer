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
#include <udisks/udisks.h>
#include <gusb.h>

G_BEGIN_DECLS

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

// TODO: make this private?
typedef struct {
	GmwDeviceState		 state;
	UDisksBlock		*udisks_block;
	GError			*error;
	gchar			*device_name;
	gchar			*device_path;
	gchar			*device_label;
	gchar			*object_path;
	gchar			*connection_id;	/* the text that identifies the port */
	gchar			*sysfs_path;
	gdouble			 complete;
	gdouble			 throughput_w;
	gdouble			 throughput_r;
	gdouble			 progress_write;
	GMutex			 mutex;
} GmwDevice;

GmwDevice	*gmw_device_new			(void);
const gchar	*gmw_device_get_icon		(GmwDevice	*device);
gchar		*gmw_device_get_description	(GmwDevice	*device);
void		 gmw_device_set_state		(GmwDevice	*device,
						 GmwDeviceState	 device_state);
void		 gmw_device_set_error		(GmwDevice	*device,
						 const GError	*error);
void		 gmw_device_set_connection_id	(GmwDevice	*device,
						 const gchar	*connection_id);
void		 gmw_device_set_device_label	(GmwDevice	*device,
						 const gchar	*device_label);
void		 gmw_device_set_udisks_drive	(GmwDevice	*device,
						 UDisksDrive	*udisks_drive);
void		 gmw_device_set_usb_device	(GmwDevice	*device,
						 GUsbDevice	*usb_device);
void		 gmw_device_free		(GmwDevice	*device);

G_END_DECLS

#endif
