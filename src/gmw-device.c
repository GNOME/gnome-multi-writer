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

#include "config.h"

#include <glib.h>
#include <glib/gi18n.h>

#include "gmw-cleanup.h"
#include "gmw-device.h"

typedef struct {
	GError			*error;		/* the error, if any */
	GMutex			 mutex;		/* mutex for the device */
	GmwDeviceState		 state;		/* the #GmwDeviceState, e.g. %GMW_DEVICE_STATE_WRITE */
	UDisksBlock		*udisks_block;	/* the #UDisksBlock device */
	gchar			*block_path;	/* path to block device, e.g. /dev/sdb */
	gchar			*hub_id;	/* hub connection */
	gchar			*hub_label;	/* hub label */
	guint8			 hub_root;	/* root hub number */
	gchar			*name;		/* name, e.g. "Hughski ColorHug" */
	gchar			*object_path;	/* UDisks object path */
	gchar			*order_display;	/* key for display sorting */
	gchar			*order_process;	/* key for processing sorting */
	gchar			*sysfs_path;	/* path in /sys */
	gdouble			 complete;	/* the amount completed, 0..1 */
	gdouble			 speed_read;	/* throughput in bytes/sec */
	gdouble			 speed_write;	/* throughput in bytes/sec */
	gdouble			 write_alloc;	/* the proportion to allocate to writing */
} GmwDevicePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GmwDevice, gmw_device, G_TYPE_OBJECT)

/**
 * gmw_device_get_state:
 **/
GmwDeviceState
gmw_device_get_state (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_val_if_fail (GMW_IS_DEVICE (device), GMW_DEVICE_STATE_UNKNOWN);
	return priv->state;
}

/**
 * gmw_device_get_udisks_block:
 **/
UDisksBlock *
gmw_device_get_udisks_block (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_val_if_fail (GMW_IS_DEVICE (device), NULL);
	return priv->udisks_block;
}

/**
 * gmw_device_get_name:
 **/
gchar *
gmw_device_get_name (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_val_if_fail (GMW_IS_DEVICE (device), NULL);
	return priv->name;
}

/**
 * gmw_device_get_block_path:
 **/
gchar *
gmw_device_get_block_path (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_val_if_fail (GMW_IS_DEVICE (device), NULL);
	return priv->block_path;
}

/**
 * gmw_device_get_hub_label:
 **/
gchar *
gmw_device_get_hub_label (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_val_if_fail (GMW_IS_DEVICE (device), NULL);
	return priv->hub_label;
}

/**
 * gmw_device_get_hub_id:
 **/
gchar *
gmw_device_get_hub_id (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_val_if_fail (GMW_IS_DEVICE (device), NULL);
	return priv->hub_id;
}

/**
 * gmw_device_get_hub_root:
 **/
guint8
gmw_device_get_hub_root (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_val_if_fail (GMW_IS_DEVICE (device), 0);
	return priv->hub_root;
}

/**
 * gmw_device_get_object_path:
 **/
gchar *
gmw_device_get_object_path (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_val_if_fail (GMW_IS_DEVICE (device), NULL);
	return priv->object_path;
}

/**
 * gmw_device_get_order_display:
 **/
gchar *
gmw_device_get_order_display (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_val_if_fail (GMW_IS_DEVICE (device), NULL);
	if (priv->order_display == NULL) {
		_cleanup_free_ gchar *key = NULL;
		key = g_strdup_printf ("%s-%s", priv->hub_id, priv->hub_label);
		gmw_device_set_order_display (device, key);
	}
	return priv->order_display;
}

/**
 * gmw_device_get_order_process:
 **/
gchar *
gmw_device_get_order_process (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_val_if_fail (GMW_IS_DEVICE (device), NULL);
	if (priv->order_process == NULL)
		return gmw_device_get_order_display (device);
	return priv->order_process;
}

/**
 * gmw_device_get_sysfs_path:
 **/
gchar *
gmw_device_get_sysfs_path (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_val_if_fail (GMW_IS_DEVICE (device), NULL);
	return priv->sysfs_path;
}

/**
 * gmw_device_get_complete:
 **/
gdouble
gmw_device_get_complete (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_val_if_fail (GMW_IS_DEVICE (device), -1.f);
	return priv->complete;
}

/**
 * gmw_device_get_speed_write:
 **/
gdouble
gmw_device_get_speed_write (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_val_if_fail (GMW_IS_DEVICE (device), -1.f);
	return priv->speed_write;
}

/**
 * gmw_device_get_speed_read:
 **/
gdouble
gmw_device_get_speed_read (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_val_if_fail (GMW_IS_DEVICE (device), -1.f);
	return priv->speed_read;
}

/**
 * gmw_device_get_size:
 **/
guint64
gmw_device_get_size (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_val_if_fail (GMW_IS_DEVICE (device), 0);
	if (priv->udisks_block == NULL)
		return 0;
	return udisks_block_get_size (priv->udisks_block);
}

/**
 * gmw_device_get_icon:
 **/
const gchar *
gmw_device_get_icon (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);

	g_return_val_if_fail (GMW_IS_DEVICE (device), NULL);

	if (priv->state == GMW_DEVICE_STATE_SUCCESS)
		return "emblem-default";
	if (priv->state == GMW_DEVICE_STATE_FAILED)
		return "drive-harddisk";
	return "drive-harddisk-usb";
}

/**
 * gmw_device_get_description:
 **/
gchar *
gmw_device_get_description (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);

	g_return_val_if_fail (GMW_IS_DEVICE (device), NULL);

	/* waiting to be written */
	if (priv->state == GMW_DEVICE_STATE_IDLE)
		return g_strdup (priv->name);

	/* failed to read or write */
	if (priv->state == GMW_DEVICE_STATE_FAILED) {
		if (priv->error == NULL) {
			/* TRANSLATORS: something internally has gone terribly
			 * wrong if the user can see this */
			return g_strdup ("Failed");
		}
		return g_strdup (priv->error->message);
	}

	/* waiting to be scheduled */
	if (priv->state == GMW_DEVICE_STATE_WAITING) {
		/* TRANSLATORS: a device is waiting to be written in a queue */
		return g_strdup ("Waiting");
	}

	/* bingo */
	if (priv->state == GMW_DEVICE_STATE_SUCCESS) {
		/* TRANSLATORS: The image has been written and verified to
		 * *one* device, not all */
		return g_strdup (_("Written successfully"));
	}

	/* write */
	if (priv->state == GMW_DEVICE_STATE_WRITE) {
		if (priv->speed_write > 0.f) {
			/* TRANSLATORS: we're writing the image to the device
			 * and we now know the speed */
			return g_strdup_printf (_("Writing at %.1f MB/s…"),
						priv->speed_write / (1000 * 1000));
		} else {
			/* TRANSLATORS: we're writing the image to the USB device */
			return g_strdup (_("Writing…"));
		}
	}

	/* read */
	if (priv->state == GMW_DEVICE_STATE_VERIFY) {
		if (priv->speed_read > 0.f) {
			/* TRANSLATORS: We're verifying the USB device contains
			 * the correct image data and we now know the speed */
			return g_strdup_printf (_("Verifying at %.1f MB/s…"),
						priv->speed_read / (1000 * 1000));
		} else {
			/* TRANSLATORS: We're verifying the USB device contains
			 * the correct image data */
			return g_strdup (_("Verifying…"));
		}
	}

	return NULL;
}

/**
 * gmw_device_set_state:
 **/
void
gmw_device_set_state (GmwDevice *device, GmwDeviceState device_state)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_if_fail (GMW_IS_DEVICE (device));
	switch (device_state) {
	case GMW_DEVICE_STATE_SUCCESS:
		priv->complete = -1.f;
		break;
	case GMW_DEVICE_STATE_IDLE:
	case GMW_DEVICE_STATE_FAILED:
		priv->speed_read = 0.f;
		priv->speed_write = 0.f;
		break;
	default:
		break;
	}
	priv->state = device_state;
}

/**
 * gmw_device_set_error:
 **/
void
gmw_device_set_error (GmwDevice *device, const GError *error)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_if_fail (GMW_IS_DEVICE (device));
	g_mutex_lock (&priv->mutex);
	if (priv->error != NULL)
		g_error_free (priv->error);
	priv->error = g_error_copy (error);
	g_mutex_unlock (&priv->mutex);
	gmw_device_set_state (device, GMW_DEVICE_STATE_FAILED);
}

/**
 * gmw_device_set_udisks_block:
 **/
void
gmw_device_set_udisks_block (GmwDevice *device, UDisksBlock *udisks_block)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_if_fail (GMW_IS_DEVICE (device));
	g_mutex_lock (&priv->mutex);
	if (priv->udisks_block != NULL)
		g_object_unref (priv->udisks_block);
	priv->udisks_block = g_object_ref (udisks_block);
	g_mutex_unlock (&priv->mutex);
}

/**
 * gmw_device_set_name:
 **/
void
gmw_device_set_name (GmwDevice *device, const gchar *name)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_if_fail (GMW_IS_DEVICE (device));
	g_mutex_lock (&priv->mutex);
	g_free (priv->name);
	priv->name = g_strdup (name);
	g_mutex_unlock (&priv->mutex);
}

/**
 * gmw_device_set_block_path:
 **/
void
gmw_device_set_block_path (GmwDevice *device, const gchar *block_path)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_if_fail (GMW_IS_DEVICE (device));
	g_mutex_lock (&priv->mutex);
	g_free (priv->block_path);
	priv->block_path = g_strdup (block_path);
	g_mutex_unlock (&priv->mutex);
}

/**
 * gmw_device_set_hub_label:
 **/
static void
gmw_device_set_hub_label (GmwDevice *device, const gchar *hub_label)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_if_fail (GMW_IS_DEVICE (device));
	g_mutex_lock (&priv->mutex);
	g_free (priv->hub_label);
	priv->hub_label = g_strdup (hub_label);
	g_mutex_unlock (&priv->mutex);
}

/**
 * gmw_device_set_hub_id:
 **/
void
gmw_device_set_hub_id (GmwDevice *device, const gchar *hub_id)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_if_fail (GMW_IS_DEVICE (device));
	g_mutex_lock (&priv->mutex);
	g_free (priv->hub_id);
	priv->hub_id = g_strdup (hub_id);
	g_mutex_unlock (&priv->mutex);
}

/**
 * gmw_device_set_object_path:
 **/
void
gmw_device_set_object_path (GmwDevice *device, const gchar *object_path)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_if_fail (GMW_IS_DEVICE (device));
	g_mutex_lock (&priv->mutex);
	g_free (priv->object_path);
	priv->object_path = g_strdup (object_path);
	g_mutex_unlock (&priv->mutex);
}

/**
 * gmw_device_set_order_display:
 **/
void
gmw_device_set_order_display (GmwDevice *device, const gchar *order_display)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_if_fail (GMW_IS_DEVICE (device));
	g_mutex_lock (&priv->mutex);
	g_free (priv->order_display);
	priv->order_display = g_strdup (order_display);
	g_mutex_unlock (&priv->mutex);
}

/**
 * gmw_device_set_order_process:
 **/
void
gmw_device_set_order_process (GmwDevice *device, const gchar *order_process)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_if_fail (GMW_IS_DEVICE (device));
	g_mutex_lock (&priv->mutex);
	g_free (priv->order_process);
	priv->order_process = g_strdup (order_process);
	g_mutex_unlock (&priv->mutex);
}

/**
 * gmw_device_set_complete_read:
 **/
void
gmw_device_set_complete_read (GmwDevice *device, gdouble complete)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_if_fail (GMW_IS_DEVICE (device));
	priv->complete = priv->write_alloc + (1.0 - priv->write_alloc) * complete;
}

/**
 * gmw_device_set_complete_write:
 **/
void
gmw_device_set_complete_write (GmwDevice *device, gdouble complete)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_if_fail (GMW_IS_DEVICE (device));
	priv->complete = priv->write_alloc * complete;
}

/**
 * gmw_device_set_speed_write:
 **/
void
gmw_device_set_speed_write (GmwDevice *device, gdouble speed_write)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_if_fail (GMW_IS_DEVICE (device));
	priv->speed_write = speed_write;
}

/**
 * gmw_device_set_speed_read:
 **/
void
gmw_device_set_speed_read (GmwDevice *device, gdouble speed_read)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_if_fail (GMW_IS_DEVICE (device));
	priv->speed_read = speed_read;
}

/**
 * gmw_device_set_write_alloc:
 **/
void
gmw_device_set_write_alloc (GmwDevice *device, gdouble write_alloc)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	g_return_if_fail (GMW_IS_DEVICE (device));
	priv->write_alloc = write_alloc;
}

/**
 * gmw_device_set_udisks_drive:
 **/
void
gmw_device_set_udisks_drive (GmwDevice *device, UDisksDrive *udisks_drive)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	const gchar *tmp;
	_cleanup_free_ gchar *sysfs_path = NULL;
	_cleanup_free_ gchar *hub_id = NULL;

	g_return_if_fail (GMW_IS_DEVICE (device));

	/* get the sibling ID, which is normally the USB path */
	tmp = udisks_drive_get_sibling_id (udisks_drive);
	if (tmp == NULL || tmp[0] == '\0')
		return;

	/* sometimes udisks goes insane */
	sysfs_path = g_path_get_dirname (tmp);
	if (g_file_test (sysfs_path, G_FILE_TEST_EXISTS)) {
		priv->sysfs_path = g_strdup (sysfs_path);
	} else {
		g_warning ("UDisks returned invalid path: %s", sysfs_path);
	}

	/* set the connection ID based on the parent device name */
	hub_id = g_path_get_basename (sysfs_path);
	gmw_device_set_hub_id (device, hub_id);
}

#if G_USB_CHECK_VERSION(0,2,4)
typedef struct {
	guint16		 parent_vid;		/* +1 */
	guint16		 parent_pid;		/* +1 */
	guint16		 grandparent_vid;	/* +2, or 0x0000 */
	guint16		 grandparent_pid;	/* +2, or 0x0000 */
	guint16		 child_vid;		/* -1, or 0x0000 */
	guint16		 child_pid;		/* -1, or 0x0000 */
	guint8		 port_number;		/* electrical, not physical */
	guint8		 chain_len;		/* internal chain number */
	const gchar	*hub_label;		/* decal on the box */
} GmwDeviceQuirk;
#endif

/**
 * gmw_device_set_usb_device_quirk:
 **/
static void
gmw_device_set_usb_device_quirk (GmwDevice *device, GUsbDevice *usb_device)
{
#if G_USB_CHECK_VERSION(0,2,4)
	guint i;
	guint j;
	_cleanup_free_ gchar *hub_id = NULL;
	_cleanup_object_unref_ GUsbDevice *usb_hub = NULL;
	_cleanup_object_unref_ GUsbDevice *usb_hub_parent = NULL;
	const GmwDeviceQuirk quirks[] = {
		/*
		 * Orico PIO Series Hub
		 *
		 *  [USB]
		 *    |
		 * [0x1a40:0x0101] --- [____0x1a40:0x0201_____]
		 *    |   |   |         |   |   |   |    |   |
		 *    #1  #2  #3        #4  #5  #6  #10  #9  #8
		 */
		{ 0x1a40, 0x0101, 0x0000, 0x0000, 0x1a40, 0x0201, 0x01, 0x00, "01" },
		{ 0x1a40, 0x0101, 0x0000, 0x0000, 0x1a40, 0x0201, 0x02, 0x00, "02" },
		{ 0x1a40, 0x0101, 0x0000, 0x0000, 0x1a40, 0x0201, 0x03, 0x00, "03" },
		{ 0x1a40, 0x0201, 0x1a40, 0x0101, 0x0000, 0x0000, 0x01, 0x01, "04" },
		{ 0x1a40, 0x0201, 0x1a40, 0x0101, 0x0000, 0x0000, 0x02, 0x01, "05" },
		{ 0x1a40, 0x0201, 0x1a40, 0x0101, 0x0000, 0x0000, 0x03, 0x01, "06" },
		{ 0x1a40, 0x0201, 0x1a40, 0x0101, 0x0000, 0x0000, 0x04, 0x01, "07" },
		{ 0x1a40, 0x0201, 0x1a40, 0x0101, 0x0000, 0x0000, 0x07, 0x01, "08" },
		{ 0x1a40, 0x0201, 0x1a40, 0x0101, 0x0000, 0x0000, 0x06, 0x01, "09" },
		{ 0x1a40, 0x0201, 0x1a40, 0x0101, 0x0000, 0x0000, 0x05, 0x01, "10" },
		/* last entry */
		{ 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, NULL }
	};

	/* is this a USB hub already */
	if (g_usb_device_get_device_class (usb_device) == 0x09) {
		usb_hub = g_object_ref (usb_device);
	} else {
		usb_hub = g_usb_device_get_parent (usb_device);
	}
	usb_hub_parent = g_usb_device_get_parent (usb_hub);
	g_debug ("Quirk info: 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%02x for 0x%04x:0x%04x",
		 g_usb_device_get_vid (usb_hub),
		 g_usb_device_get_pid (usb_hub),
		 usb_hub_parent ? g_usb_device_get_vid (usb_hub_parent) : 0x0,
		 usb_hub_parent ? g_usb_device_get_pid (usb_hub_parent) : 0x0,
		 g_usb_device_get_port_number (usb_device),
		 g_usb_device_get_vid (usb_device),
		 g_usb_device_get_pid (usb_device));

	for (i = 0; quirks[i].hub_label != NULL; i++) {
		/* check grandparent */
		if (usb_hub_parent != NULL && quirks[i].grandparent_vid != 0x0000) {
			if (quirks[i].grandparent_vid != g_usb_device_get_vid (usb_hub_parent))
				continue;
			if (quirks[i].grandparent_pid != g_usb_device_get_pid (usb_hub_parent))
				continue;
		}

		/* check parent */
		if (usb_hub != NULL && quirks[i].parent_vid != 0x0000) {
			if (quirks[i].parent_vid != g_usb_device_get_vid (usb_hub))
				continue;
			if (quirks[i].parent_pid != g_usb_device_get_pid (usb_hub))
				continue;
		}

		/* check children */
		if (usb_hub != NULL && quirks[i].child_vid != 0x0000) {
			GUsbDevice *tmp;
			gboolean child_exists = FALSE;
			_cleanup_ptrarray_unref_ GPtrArray *children = NULL;

			/* the specified child just has to exist once */
			children = g_usb_device_get_children (usb_hub);
			for (j = 0; j < children->len; j++) {
				tmp = g_ptr_array_index (children, j);
				if (g_usb_device_get_vid (tmp) == quirks[i].child_vid &&
				    g_usb_device_get_pid (tmp) == quirks[i].child_pid) {
					child_exists = TRUE;
					break;
				}
			}
			if (!child_exists)
				continue;
		}

		/* check port number */
		if (quirks[i].port_number != 0x00) {
			if (quirks[i].port_number != g_usb_device_get_port_number (usb_device))
				continue;
		}

		/* set the decal name */
		gmw_device_set_hub_label (device, quirks[i].hub_label);

		/* get the top-level port address */
		if (usb_hub_parent != NULL && quirks[i].chain_len == 1) {
			hub_id = g_strdup_printf ("%02x:%02x",
						g_usb_device_get_bus (usb_hub_parent),
						g_usb_device_get_address (usb_hub_parent));
		} else {
			hub_id = g_strdup_printf ("%02x:%02x",
						g_usb_device_get_bus (usb_hub),
						g_usb_device_get_address (usb_hub));
		}
		gmw_device_set_hub_id (device, hub_id);
		break;
	}
#endif
}

/**
 * gmw_device_set_usb_device:
 **/
void
gmw_device_set_usb_device (GmwDevice *device, GUsbDevice *usb_device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	const gchar *tmp;

	g_return_if_fail (GMW_IS_DEVICE (device));

	/* use the bare platform ID by default */
	tmp = g_usb_device_get_platform_id (usb_device);
	if (tmp != NULL)
		gmw_device_set_hub_id (device, tmp + 7);

	/* get the USB root hub number */
	priv->hub_root = g_usb_device_get_bus (usb_device);

	/* can we get the ID from a quirk */
	gmw_device_set_usb_device_quirk (device, usb_device);
}

static void
gmw_device_finalize (GObject *object)
{
	GmwDevice *device;
	GmwDevicePrivate *priv;

	g_return_if_fail (GMW_IS_DEVICE (object));

	device = GMW_DEVICE (object);
	priv = gmw_device_get_instance_private (device);

	g_mutex_clear (&priv->mutex);
	if (priv->error != NULL)
		g_error_free (priv->error);
	g_free (priv->block_path);
	g_free (priv->hub_id);
	g_free (priv->hub_label);
	g_free (priv->name);
	g_free (priv->object_path);
	g_free (priv->order_display);
	g_free (priv->order_process);
	g_free (priv->sysfs_path);
	g_object_unref (priv->udisks_block);

	G_OBJECT_CLASS (gmw_device_parent_class)->finalize (object);
}

static void
gmw_device_class_init (GmwDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = gmw_device_finalize;
}

static void
gmw_device_init (GmwDevice *device)
{
	GmwDevicePrivate *priv = gmw_device_get_instance_private (device);
	priv->state = GMW_DEVICE_STATE_IDLE;
	priv->complete = -1.f;
	priv->speed_read = -1.f;
	priv->speed_write = -1.f;
	priv->write_alloc = 1.f;
	priv->hub_id = g_strdup ("???");
	g_mutex_init (&priv->mutex);
}

/**
 * gmw_device_new:
 **/
GmwDevice *
gmw_device_new (void)
{
	GmwDevice *device;
	device = g_object_new (GMW_TYPE_DEVICE, NULL);
	return GMW_DEVICE (device);
}
