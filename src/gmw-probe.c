/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gi18n.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>
#include <gudev/gudev.h>
#include <linux/fs.h>
#include <linux/usbdevice_fs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "gmw-cleanup.h"

#define ONE_BLOCK		0x8000
#define ONE_MB			0x100000
#define ONE_GB			0x40000000

typedef struct {
	guint8			*data_old;
	guint8			*data_random;
	gssize			 bytes_read;
	gssize			 bytes_wrote;
	guint64			 address;
	guint64			 offset;
	gboolean		 valid;
} GmwProbeBlock;

typedef struct {
	guint64			 disk_size;
	int			 fd;
	gchar			*block_dev;
	GPtrArray		*data_save;
	GUdevDevice		*udev_device;
} GmwProbeDevice;

static guint32 seed = 0;

#define GMW_ERROR		1
#define GMW_ERROR_FAILED	0
#define GMW_ERROR_IS_FAKE	1

/* The number of chunks of data to read and write to
 * verify integrity */
#define NUM_CHUNKS 256

static guint8 *
gmw_probe_get_random_data (guint len)
{
	guint8 *data = g_new (guint8, len);
	for (guint i = 0; i < len; i++)
		data[i] = g_random_int_range ('a', 'z');
	return data;
}

static void
gmw_probe_block_free (GmwProbeBlock *item)
{
	g_free (item->data_old);
	g_free (item->data_random);
	g_free (item);
}

static gboolean
gmw_probe_device_reset (GmwProbeDevice *dev, GError **error)
{
	int fd;
	const gchar *devnode;

	/* just reset device */
	devnode = g_udev_device_get_device_file (dev->udev_device);
	g_debug ("Resetting %s", devnode);
	fd = g_open (devnode, O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		g_set_error (error,
			     GMW_ERROR,
			     GMW_ERROR_FAILED,
			     "Failed to open %s", devnode);
		return FALSE;
	}
	if (ioctl (fd, USBDEVFS_RESET) != 0) {
		g_set_error (error,
			     GMW_ERROR,
			     GMW_ERROR_FAILED,
			     "Failed to reset device");
		close (fd);
		return FALSE;
	}
	close (fd);
	return TRUE;
}

static gboolean
gmw_probe_device_open (GmwProbeDevice *dev, GError **error)
{
	dev->fd = g_open (dev->block_dev, O_RDWR | O_SYNC);
	if (dev->fd < 0) {
		g_set_error (error,
			     GMW_ERROR,
			     GMW_ERROR_FAILED,
			     "Failed to open %s", dev->block_dev);
		return FALSE;
	}

	/* do not use the OS cache */
	if (posix_fadvise (dev->fd, 0, 0,
			   POSIX_FADV_DONTNEED |
			   POSIX_FADV_RANDOM |
			   POSIX_FADV_NOREUSE) != 0) {
		g_warning ("Unable to call fadvise on %s", dev->block_dev);
	}
	return TRUE;
}

static void
gmw_probe_device_free (GmwProbeDevice *dev)
{
	if (dev->fd > 0)
		close (dev->fd);
	if (dev->udev_device != NULL)
		g_object_unref (dev->udev_device);
	g_free (dev->block_dev);
	g_ptr_array_unref (dev->data_save);
	g_free (dev);
}

static gsize
gmw_probe_device_read (GmwProbeDevice *dev, guint64 addr, guint8 *buf, gssize len)
{
	gsize bytes_read;
	if (lseek (dev->fd, addr, SEEK_SET) < 0)
		return 0;
	bytes_read = read (dev->fd, buf, len);
	g_debug ("read %" G_GSIZE_FORMAT " @ %" G_GUINT64_FORMAT "MB",
		 bytes_read, addr / ONE_MB);
	return bytes_read;
}

static gsize
gmw_probe_device_write (GmwProbeDevice *dev, guint64 addr, const guint8 *buf, gssize len)
{
	gsize bytes_written;
	if (lseek (dev->fd, addr, SEEK_SET) < 0)
		return 0;
	bytes_written = write (dev->fd, buf, len);
	g_debug ("wrote %" G_GSIZE_FORMAT " @ %" G_GUINT64_FORMAT "MB",
		 bytes_written, addr / ONE_MB);
	return bytes_written;
}

static gboolean
gmw_probe_device_data_save (GmwProbeDevice *dev,
			    GCancellable *cancellable,
			    GError **error)
{
	/* aim for roughtly the same number of chunks for all device sizes */
	guint64 chunk_size = dev->disk_size / NUM_CHUNKS;
	g_debug ("using chunk size of %" G_GUINT64_FORMAT "MB",
		 chunk_size / ONE_MB);
	for (guint i = 1; i < NUM_CHUNKS; i++) {
		GmwProbeBlock *item = g_new0 (GmwProbeBlock, 1);
		item->valid = TRUE;
		item->offset = g_random_int_range (1, 0xff);
		item->address = i * chunk_size;
		item->data_old = g_new0 (guint8, ONE_BLOCK);
		if (item->address >= dev->disk_size) {
			gmw_probe_block_free (item);
			break;
		}
		item->data_random = gmw_probe_get_random_data (ONE_BLOCK);
		item->bytes_read = gmw_probe_device_read (dev,
							  item->address +
							  item->offset,
							  item->data_old,
							  ONE_BLOCK);
		g_ptr_array_add (dev->data_save, item);
		if (item->bytes_read != ONE_BLOCK)
			break;
		if (g_cancellable_set_error_if_cancelled (cancellable, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
gmw_probe_device_data_set_dummy (GmwProbeDevice *dev,
				 GCancellable *cancellable,
				 GError **error)
{
	for (guint i = 0; i < dev->data_save->len; i++) {
		GmwProbeBlock *item = g_ptr_array_index (dev->data_save, i);
		item->bytes_wrote = gmw_probe_device_write (dev,
							    item->address +
							    item->offset,
							    item->data_random,
							    ONE_BLOCK);

		if (item->bytes_wrote != ONE_BLOCK)
			break;
		if (g_cancellable_set_error_if_cancelled (cancellable, error))
			return FALSE;
	}

	return TRUE;
}

static gboolean
gmw_probe_device_data_verify (GmwProbeDevice *dev,
			      GCancellable *cancellable,
			      GError **error)
{
	guint i;
	g_autofree guint8 *wbuf2 = g_new (guint8, ONE_BLOCK + 0xff);
	for (i = 0; i < dev->data_save->len; i++) {
		GmwProbeBlock *item = g_ptr_array_index (dev->data_save, i);

		/* use a random offset to confuse drives that are just saving
		 * the address and data in some phantom FAT */
		guint32 offset = g_random_int_range (1, 0xff);
		item->bytes_read = gmw_probe_device_read (dev,
							  item->address +
							  item->offset - offset,
							  wbuf2,
							  ONE_BLOCK + offset);
		if (item->bytes_read != ONE_BLOCK + offset) {
			g_set_error (error,
				     GMW_ERROR,
				     GMW_ERROR_FAILED,
				     "Failed to read data (seed: %u)",
				     seed);
			return FALSE;
		}
		item->valid = memcmp (item->data_random,
				      wbuf2 + offset,
				      ONE_BLOCK) == 0;
		if (g_cancellable_set_error_if_cancelled (cancellable, error))
			return FALSE;

		/* optimize; we don't need to check any more */
		if (!item->valid)
			break;
	}

	/* if we aborted early, the rest of the drive is junk */
	for (i = i; i < dev->data_save->len; i++) {
		GmwProbeBlock *item = g_ptr_array_index (dev->data_save, i);
		item->valid = FALSE;
	}

	return TRUE;
}

static gboolean
gmw_probe_device_data_restore (GmwProbeDevice *dev,
			       GCancellable *cancellable,
			       GError **error)
{
	for (guint i = 0; i < dev->data_save->len; i++) {
		GmwProbeBlock *item = g_ptr_array_index (dev->data_save, i);
		if (!item->valid)
			continue;
		item->bytes_wrote = gmw_probe_device_write (dev,
							    item->address +
							    item->offset,
							    item->data_old,
							    ONE_BLOCK);
		if (item->bytes_wrote != ONE_BLOCK)
			break;
		if (g_cancellable_set_error_if_cancelled (cancellable, error))
			return FALSE;
	}

	return TRUE;
}

static gboolean
gmw_probe_scan_device (GmwProbeDevice *dev, GCancellable *cancellable, GError **error)
{
	/* open block device */
	if (!gmw_probe_device_open (dev, error))
		return FALSE;

	/* get reported size */
	if (ioctl (dev->fd, BLKGETSIZE64, &dev->disk_size) != 0) {
		g_set_error (error,
			     GMW_ERROR,
			     GMW_ERROR_FAILED,
			     "Failed to get reported size");
		return FALSE;
	}
	if (dev->disk_size == 0) {
		g_set_error_literal (error,
				     GMW_ERROR,
				     GMW_ERROR_FAILED,
				     "Disk capacity reported as zero");
		return FALSE;
	}
	if (dev->disk_size > 0x4000000000llu) {
		g_set_error (error,
			     GMW_ERROR,
			     GMW_ERROR_FAILED,
			     "Disk capacity reported as invalid: %"
			     G_GUINT64_FORMAT "GB",
			     dev->disk_size / ONE_GB);
		return FALSE;
	}
	g_debug ("Disk reports to be %" G_GUINT64_FORMAT "MB in size",
		 dev->disk_size / ONE_MB);

	/* save data that's there already */
	if (!gmw_probe_device_data_save (dev, cancellable, error))
		return FALSE;

	/* write 32k or random to every 32Mb */
	if (!gmw_probe_device_data_set_dummy (dev, cancellable, error)) {
		gmw_probe_device_data_restore (dev, cancellable, NULL);
		return FALSE;
	}

	/* sanity check for really broken devices */
	for (guint i = 0; i < dev->data_save->len; i++) {
		GmwProbeBlock *item = g_ptr_array_index (dev->data_save, i);
		if (item->bytes_read != item->bytes_wrote) {
			g_set_error (error,
				     GMW_ERROR,
				     GMW_ERROR_FAILED,
				     "Failed to write len at %" G_GUINT64_FORMAT "MB",
				     item->address / ONE_MB);
			gmw_probe_device_data_restore (dev, cancellable, NULL);
			return FALSE;
		}
	}

	/* reset device */
	if (!gmw_probe_device_reset (dev, error)) {
		gmw_probe_device_data_restore (dev, cancellable, NULL);
		return FALSE;
	}

	/* wait for block drive to reappear */
	close (dev->fd);
	if (!gmw_probe_device_open (dev, error))
		return FALSE;

	/* read each chunk in again */
	if (!gmw_probe_device_data_verify (dev, cancellable, error)) {
		gmw_probe_device_data_restore (dev, cancellable, NULL);
		return FALSE;
	}

	/* write back original data */
	if (!gmw_probe_device_data_restore (dev, cancellable, error))
		return FALSE;

	/* get results */
	for (guint i = 0; i < dev->data_save->len; i++) {
		GmwProbeBlock *item = g_ptr_array_index (dev->data_save, i);
		if (!item->valid) {
			g_set_error (error,
				     GMW_ERROR,
				     GMW_ERROR_IS_FAKE,
				     "Failed to verify data at %" G_GUINT64_FORMAT "MB  (seed: %u)",
				     item->address / ONE_MB,
				     seed);
			return FALSE;
		}
	}
	return TRUE;
}

static gboolean
gmw_probe_use_device (GUdevClient *udev_client,
		      const gchar *block_dev,
		      GCancellable *cancellable,
		      GError **error)
{
	_cleanup_object_unref_ GUdevDevice *udev_device = NULL;
	GmwProbeDevice *dev;

	/* create worker object */
	dev = g_new0 (GmwProbeDevice, 1);
	dev->block_dev = g_strdup (block_dev);
	dev->data_save = g_ptr_array_new_with_free_func ((GDestroyNotify) gmw_probe_block_free);

	/* find udev device */
	udev_device = g_udev_client_query_by_device_file (udev_client, block_dev);
	if (udev_device == NULL) {
		g_set_error (error,
			     GMW_ERROR,
			     GMW_ERROR_FAILED,
			     "Failed to find %s", block_dev);
		gmw_probe_device_free (dev);
		return FALSE;
	}
	dev->udev_device = g_udev_device_get_parent_with_subsystem (udev_device,
								    "usb",
								    "usb_device");
	if (dev->udev_device == NULL) {
		g_set_error_literal (error,
				     GMW_ERROR,
				     GMW_ERROR_FAILED,
				     "Not a USB device");
		gmw_probe_device_free (dev);
		return FALSE;
	}

	/* actually do the scanning now */
	if (!gmw_probe_scan_device (dev, cancellable, error)) {
		gmw_probe_device_free (dev);
		return FALSE;
	}

	/* success */
	gmw_probe_device_free (dev);
	return TRUE;
}

static gboolean
gmw_probe_is_block_device_valid (const gchar *block_device)
{
	/* dev prefix */
	if (!g_str_has_prefix (block_device, "/dev/"))
		return FALSE;

	/* has no partition number */
	for (guint i = 5; block_device[i] != '\0'; i++) {
		if (g_ascii_isdigit (block_device[i]))
			return FALSE;
	}
	return TRUE;
}


static gboolean
gmw_probe_is_block_device_mounted (const gchar *block_device)
{
	g_autofree gchar *data = NULL;
	if (!g_file_get_contents ("/etc/mtab", &data, NULL, NULL))
		return FALSE;
	return g_strrstr (data, block_device) != NULL;
}

int
main (int argc, char **argv)
{
	const gchar *subsystems[] = { "usb", NULL };
	gboolean verbose = FALSE;
	_cleanup_object_unref_ GUdevClient *udev_client = NULL;
	g_autoptr(GCancellable) cancellable = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GOptionContext) context = NULL;

	const GOptionEntry options[] = {
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
			/* TRANSLATORS: command line option */
			_("Show extra debugging information"), NULL },
		{ "seed", 's', 0, G_OPTION_ARG_INT, &seed,
			/* TRANSLATORS: command line option */
			_("Random seed for predictability"), NULL },
		{ NULL}
	};

	if (seed == 0)
		seed = g_random_int_range (0, G_MAXINT);
	g_random_set_seed (seed);
	g_debug ("Using %u as a random seed", seed);

	/* TRANSLATORS: A program to copy the LiveUSB image onto USB hardware */
	context = g_option_context_new (NULL);
	g_option_context_add_main_entries (context, options, NULL);
	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		g_print ("Failed to parse command line: %s\n", error->message);
		return EXIT_FAILURE;
	}

	if (verbose)
		g_setenv ("G_MESSAGES_DEBUG", "all", TRUE);

	/* valid arguments */
	if (argc != 2 || !gmw_probe_is_block_device_valid (argv[1])) {
		g_print ("Block device required as argument\n");
		return EXIT_FAILURE;
	}

	/* already mounted */
	if (gmw_probe_is_block_device_mounted (argv[1])) {
		g_print ("Partition mounted from block device\n");
		return EXIT_FAILURE;
	}

	/* probe device */
	cancellable = g_cancellable_new ();
	udev_client = g_udev_client_new (subsystems);
	if (!gmw_probe_use_device (udev_client, argv[1], cancellable, &error)) {
		if (g_error_matches (error, GMW_ERROR, GMW_ERROR_IS_FAKE)) {
			g_print ("Device is FAKE: %s\n", error->message);
		} else {
			g_print ("Failed to scan device: %s\n", error->message);
		}
		return EXIT_FAILURE;
	}
	g_print ("Device is GOOD\n");
	return EXIT_SUCCESS;
}
