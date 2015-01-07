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
 * gmw_device_new:
 **/
GmwDevice *
gmw_device_new (void)
{
	GmwDevice *device;
	device = g_new0 (GmwDevice, 1);
	device->state = GMW_DEVICE_STATE_IDLE;
	device->complete = -1.f;
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
	g_free (device->sibling_id);
	g_object_unref (device->udisks_block);
	g_free (device);
}
