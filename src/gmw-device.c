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

#include <glib/gi18n.h>

#include "gmw-cleanup.h"
#include "gmw-device.h"

/**
 * gmw_device_get_icon:
 **/
const gchar *
gmw_device_get_icon (GmwDevice *device)
{
	if (device->state == GMW_DEVICE_STATE_SUCCESS)
		return "emblem-default";
	if (device->state == GMW_DEVICE_STATE_FAILED)
		return "drive-harddisk";
	return "drive-harddisk-usb";
}

/**
 * gmw_device_get_description:
 **/
gchar *
gmw_device_get_description (GmwDevice *device)
{
	/* waiting to be written */
	if (device->state == GMW_DEVICE_STATE_IDLE)
		return g_strdup (device->device_name);

	/* failed to read or write */
	if (device->state == GMW_DEVICE_STATE_FAILED) {
		if (device->error == NULL) {
			/* TRANSLATORS: something internally has gone terribly
			 * wrong if the user can see this */
			return g_strdup ("Failed");
		}
		return g_strdup (device->error->message);
	}

	/* waiting to be scheduled */
	if (device->state == GMW_DEVICE_STATE_WAITING) {
		/* TRANSLATORS: a device is waiting to be written in a queue */
		return g_strdup ("Waiting");
	}

	/* bingo */
	if (device->state == GMW_DEVICE_STATE_SUCCESS) {
		/* TRANSLATORS: The image has been written and verified to
		 * *one* device, not all */
		return g_strdup (_("Written successfully"));
	}

	/* write */
	if (device->state == GMW_DEVICE_STATE_WRITE) {
		if (device->throughput_w > 0.f) {
			/* TRANSLATORS: we're writing the image to the device
			 * and we now know the speed */
			return g_strdup_printf (_("Writing at %.1f MB/s…"),
						device->throughput_w / (1000 * 1000));
		} else {
			/* TRANSLATORS: we're writing the image to the USB device */
			return g_strdup (_("Writing…"));
		}
	}

	/* read */
	if (device->state == GMW_DEVICE_STATE_VERIFY) {
		if (device->throughput_r > 0.f) {
			/* TRANSLATORS: We're verifying the USB device contains
			 * the correct image data and we now know the speed */
			return g_strdup_printf (_("Verifying at %.1f MB/s…"),
						device->throughput_r / (1000 * 1000));
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
	switch (device_state) {
	case GMW_DEVICE_STATE_SUCCESS:
		device->complete = -1.f;
		break;
	case GMW_DEVICE_STATE_IDLE:
	case GMW_DEVICE_STATE_FAILED:
		device->throughput_r = 0.f;
		device->throughput_w = 0.f;
		break;
	default:
		break;
	}
	device->state = device_state;
}

/**
 * gmw_device_set_error:
 **/
void
gmw_device_set_error (GmwDevice *device, const GError *error)
{
	g_mutex_lock (&device->mutex);
	if (device->error != NULL)
		g_error_free (device->error);
	device->error = g_error_copy (error);
	g_mutex_unlock (&device->mutex);
	gmw_device_set_state (device, GMW_DEVICE_STATE_FAILED);
}

/**
 * gmw_device_set_connection_id:
 **/
void
gmw_device_set_connection_id (GmwDevice *device, const gchar *connection_id)
{
	g_mutex_lock (&device->mutex);
	g_free (device->connection_id);
	device->connection_id = g_strdup (connection_id);
	g_mutex_unlock (&device->mutex);
}

/**
 * gmw_device_set_device_label:
 **/
void
gmw_device_set_device_label (GmwDevice *device, const gchar *device_label)
{
	g_mutex_lock (&device->mutex);
	g_free (device->device_label);
	device->device_label = g_strdup (device_label);
	g_mutex_unlock (&device->mutex);
}

/**
 * gmw_device_set_udisks_drive:
 **/
void
gmw_device_set_udisks_drive (GmwDevice *device, UDisksDrive *udisks_drive)
{
	const gchar *tmp;
	_cleanup_free_ gchar *sysfs_path = NULL;
	_cleanup_free_ gchar *connection_id = NULL;

	/* get the sibling ID, which is normally the USB path */
	tmp = udisks_drive_get_sibling_id (udisks_drive);
	if (tmp == NULL || tmp[0] == '\0')
		return;

	/* sometimes udisks goes insane */
	sysfs_path = g_path_get_dirname (tmp);
	if (g_file_test (sysfs_path, G_FILE_TEST_EXISTS)) {
		device->sysfs_path = g_strdup (sysfs_path);
	} else {
		g_warning ("UDisks returned invalid path: %s", sysfs_path);
	}

	/* set the connection ID based on the parent device name */
	connection_id = g_path_get_basename (sysfs_path);
	gmw_device_set_connection_id (device, connection_id);
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
	const gchar	*device_label;		/* decal on the box */
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
	_cleanup_free_ gchar *connection_id = NULL;
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

	for (i = 0; quirks[i].device_label != NULL; i++) {
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
		gmw_device_set_device_label (device, quirks[i].device_label);

		/* get the top-level port address */
		if (usb_hub_parent != NULL && quirks[i].chain_len == 1) {
			connection_id = g_strdup_printf ("%02x:%02x",
						g_usb_device_get_bus (usb_hub_parent),
						g_usb_device_get_address (usb_hub_parent));
		} else {
			connection_id = g_strdup_printf ("%02x:%02x",
						g_usb_device_get_bus (usb_hub),
						g_usb_device_get_address (usb_hub));
		}
		gmw_device_set_connection_id (device, connection_id);
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
	const gchar *tmp;

	/* use the bare platform ID by default */
	tmp = g_usb_device_get_platform_id (usb_device);
	if (tmp != NULL)
		gmw_device_set_connection_id (device, tmp + 7);

	/* can we get the ID from a quirk */
	gmw_device_set_usb_device_quirk (device, usb_device);
}

/**
 * gmw_device_new:
 **/
GmwDevice *
gmw_device_new (void)
{
	GmwDevice *device;
	device = g_new0 (GmwDevice, 1);
	device->state = GMW_DEVICE_STATE_IDLE;
	device->complete = -1.f;
	device->connection_id = g_strdup ("???");
	return device;
}

/**
 * gmw_device_free:
 **/
void
gmw_device_free (GmwDevice *device)
{
	g_mutex_clear (&device->mutex);
	if (device->error != NULL)
		g_error_free (device->error);
	g_free (device->device_name);
	g_free (device->device_path);
	g_free (device->object_path);
	g_free (device->connection_id);
	g_free (device->device_label);
	g_free (device->sysfs_path);
	g_object_unref (device->udisks_block);
	g_free (device);
}
