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

#include <canberra-gtk.h>
#include <gio/gunixfdlist.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gusb.h>
#include <locale.h>
#include <stdlib.h>
#include <udisks/udisks.h>

#include "gmw-cleanup.h"
#include "gmw-device.h"

typedef struct {
	GFile			*image_file;
	guint64			 image_file_size;
	GPtrArray		*devices;
	GMutex			 devices_mutex;
	GSettings		*settings;
	GtkApplication		*application;
	GtkBuilder		*builder;
	GCancellable		*cancellable;
	GUsbContext		*usb_ctx;
	UDisksClient		*udisks_client;
	GThreadPool		*thread_pool;
	GMutex			 thread_pool_mutex;
	gboolean		 done_polkit_auth;
	guint			 inhibit_id;
	guint			 throughput_id;
	guint			 idle_id;
	GMutex			 idle_id_mutex;
	GtkWidget		*switch_verify;
	GtkWidget		*switch_blank;
} GmwPrivate;

/**
 * gmw_error_dialog:
 **/
static void
gmw_error_dialog (GmwPrivate *priv, const gchar *title, const gchar *message)
{
	GtkWindow *window;
	GtkWidget *dialog;

	window = GTK_WINDOW(gtk_builder_get_object (priv->builder, "dialog_main"));
	dialog = gtk_message_dialog_new (window,
					 GTK_DIALOG_MODAL,
					 GTK_MESSAGE_ERROR,
					 GTK_BUTTONS_CLOSE,
					 "%s", title);
	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
						  "%s", message);
	gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
}

/**
 * gmw_activate_cb:
 **/
static void
gmw_activate_cb (GApplication *application, GmwPrivate *priv)
{
	GtkWindow *window;
	window = GTK_WINDOW (gtk_builder_get_object (priv->builder, "dialog_main"));
	gtk_window_present (window);
}

/**
 * gmw_cancel_clicked_cb:
 **/
static void
gmw_cancel_clicked_cb (GtkWidget *widget, GmwPrivate *priv)
{
	g_cancellable_cancel (priv->cancellable);
}

/**
 * gmw_devices_sort_cb:
 **/
static gint
gmw_devices_sort_cb (gconstpointer a, gconstpointer b)
{
	GmwDevice *device_a = *((GmwDevice **) a);
	GmwDevice *device_b = *((GmwDevice **) b);
	return g_strcmp0 (device_a->sibling_id, device_b->sibling_id);
}

/**
 * gmw_refresh_ui:
 **/
static void
gmw_refresh_ui (GmwPrivate *priv)
{
	GmwDevice *device;
	GtkWidget *grid;
	GtkWidget *w;
	const guint max_devices_per_column = 10;
	guint i;

	/* remove old children */
	grid = GTK_WIDGET (gtk_builder_get_object (priv->builder, "grid_status"));
	gtk_container_foreach (GTK_CONTAINER (grid),
			       (GtkCallback) gtk_widget_destroy, priv);

	/* add new children */
	for (i = 0; i < priv->devices->len; i++) {
		_cleanup_free_ gchar *sibling_markup = NULL;
		_cleanup_free_ gchar *title = NULL;
		guint row = i % max_devices_per_column;
		guint col = (i / max_devices_per_column) * 4;

		device = g_ptr_array_index (priv->devices, i);
		g_mutex_lock (&device->mutex);

		/* add sibling-id */
		w = gtk_label_new (NULL);
		if (col > 0)
			gtk_widget_set_margin_start (w, 30);
		sibling_markup = g_strdup_printf ("<tt><small>%s</small></tt>",
						  device->sibling_id);
		gtk_label_set_markup (GTK_LABEL (w), sibling_markup);
		g_object_set (w, "xalign", 1.f, NULL);
		gtk_grid_attach (GTK_GRID (grid), w, col + 0, row, 1, 1);

		/* add icon */
		w = gtk_image_new ();
		gtk_image_set_from_icon_name (GTK_IMAGE (w),
					      gmw_device_get_icon (device),
					      GTK_ICON_SIZE_BUTTON);
		gtk_grid_attach (GTK_GRID (grid), w, col + 1, row, 1, 1);

		/* add optional progressbar */
		if (device->complete > 0.f) {
			w = gtk_progress_bar_new ();
			gtk_widget_set_valign (w, GTK_ALIGN_CENTER);
			gtk_grid_attach (GTK_GRID (grid), w, col + 2, row, 1, 1);
			gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (w), device->complete);
		}

		/* add optional status text */
		title = gmw_device_get_description (device);
		if (title != NULL) {
			w = gtk_label_new (title);
			g_object_set (w, "xalign", 0.f, NULL);
			gtk_label_set_width_chars (GTK_LABEL (w), 20);
			gtk_grid_attach (GTK_GRID (grid), w, col + 3, row, 1, 1);
		}

		g_mutex_unlock (&device->mutex);
	}

	gtk_widget_show_all (grid);

	/* allow changing settings */
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_settings"));
	gtk_widget_set_sensitive (w, g_thread_pool_get_num_threads (priv->thread_pool) == 0);

	/* update buttons */
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_cancel"));
	gtk_widget_set_visible (w, g_thread_pool_get_num_threads (priv->thread_pool) > 0);
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_start"));
	gtk_widget_set_visible (w, g_thread_pool_get_num_threads (priv->thread_pool) == 0 &&
				   priv->devices->len > 0);

	/* set stack */
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "stack_main"));
	gtk_stack_set_visible_child_name (GTK_STACK (w), priv->devices->len > 0 ? "status" : "usb");
}

/**
 * gmw_refresh_in_idle_cb:
 **/
static gboolean
gmw_refresh_in_idle_cb (gpointer user_data)
{
	GmwPrivate *priv = (GmwPrivate *) user_data;
	gmw_refresh_ui (priv);

	/* we've been run */
	g_mutex_lock (&priv->idle_id_mutex);
	priv->idle_id = 0;
	g_mutex_unlock (&priv->idle_id_mutex);
	return FALSE;
}

/**
 * gmw_refresh_in_idle:
 **/
static void
gmw_refresh_in_idle (GmwPrivate *priv)
{
	g_mutex_lock (&priv->idle_id_mutex);
	if (priv->idle_id != 0)
		g_source_remove (priv->idle_id);
	priv->idle_id = g_timeout_add (50, gmw_refresh_in_idle_cb, priv);
	g_mutex_unlock (&priv->idle_id_mutex);
}

/**
 * gmw_copy_done:
 **/
static void
gmw_copy_done (GmwPrivate *priv)
{
	g_mutex_lock (&priv->thread_pool_mutex);
	if (g_thread_pool_get_num_threads (priv->thread_pool) == 1) {
		ca_context_play (ca_gtk_context_get (), 0,
				 CA_PROP_EVENT_ID, "complete",
				 /* TRANSLATORS: the application name */
				 CA_PROP_APPLICATION_NAME, _("GNOME MultiWriter"),
				 /* TRANSLATORS: the success sound description */
				 CA_PROP_EVENT_DESCRIPTION, _("Image written successfully"),
				 NULL);

		/* allow suspend now */
		if (priv->inhibit_id != 0) {
			gtk_application_uninhibit (priv->application, priv->inhibit_id);
			priv->inhibit_id = 0;
		}
		if (priv->throughput_id > 0) {
			g_source_remove (priv->throughput_id);
			priv->throughput_id = 0;
		}
	}
	g_mutex_unlock (&priv->thread_pool_mutex);
}

/**
 * gmw_get_aligned_buffer:
 **/
static guchar *
gmw_get_aligned_buffer (gint buffer_size, guchar **buffer)
{
	long page_size;
	guchar *buffer_unaligned;

	/* taken from gdurestorediskimagedialog.c */
	page_size = sysconf (_SC_PAGESIZE);
	buffer_unaligned = g_new0 (guchar, buffer_size + page_size);
	*buffer = (guchar *) (((gintptr) (buffer_unaligned + page_size)) & (~(page_size - 1)));
	return buffer_unaligned;
}

/**
 * gmw_device_write:
 **/
static gboolean
gmw_device_write (GmwPrivate *priv,
		  GmwDevice *device,
		  GInputStream *image_stream,
		  GCancellable *cancellable,
		  GError **error)
{
	const gint buffer_size = (1 * 1024 * 1024); /* default to 1 MiB blocks */
	gboolean fill_zeros = FALSE;
	gboolean ret = FALSE;
	gint fd = -1;
	guchar *buffer = NULL;
	guint64 bytes_completed = 0;
	guint64 bytes_throughput = 0;
	guint64 bytes_total;
	_cleanup_error_free_ GError *error_local = NULL;
	_cleanup_free_ guchar *buffer_unaligned = NULL;
	_cleanup_object_unref_ GOutputStream *device_stream = NULL;
	_cleanup_object_unref_ GUnixFDList *fd_list = NULL;
	_cleanup_timer_destroy_ GTimer *timer = NULL;
	_cleanup_variant_unref_ GVariant *fd_index = NULL;

	/* get fd from udisks */
	if (!udisks_block_call_open_for_restore_sync (device->udisks_block,
						      g_variant_new ("a{sv}", NULL), /* options */
						      NULL, /* fd_list */
						      &fd_index,
						      &fd_list,
						      cancellable,
						      error)) {
		if (error != NULL)
			g_dbus_error_strip_remote_error (*error);
		goto out;
	}
	fd = g_unix_fd_list_get (fd_list, g_variant_get_handle (fd_index), error);
	if (fd == -1)
		goto out;
	device_stream = g_unix_output_stream_new (fd, TRUE);

	/* read huge blocks and write it to the output device */
	buffer_unaligned = gmw_get_aligned_buffer (buffer_size, &buffer);
	bytes_completed = 0;
	timer = g_timer_new ();

	/* get the total number of bytes we need to image */
	bytes_total = priv->image_file_size;
	if (g_settings_get_boolean (priv->settings, "blank-drive")) {
		bytes_total = udisks_block_get_size (device->udisks_block);
		fill_zeros = TRUE;
	}
	gmw_device_set_state (device, GMW_DEVICE_STATE_WRITE);
	while (bytes_completed < bytes_total) {
		gdouble elapsed;
		gsize bytes_to_read;
		gsize bytes_read;
		gssize bytes_written;

		bytes_to_read = buffer_size;
		if (bytes_to_read + bytes_completed > bytes_total)
			bytes_to_read = bytes_total - bytes_completed;

		if (!g_input_stream_read_all (image_stream,
					      buffer,
					      bytes_to_read,
					      &bytes_read,
					      cancellable,
					      &error_local)) {
			if (g_error_matches (error_local,
					     G_IO_ERROR,
					     G_IO_ERROR_CANCELLED)) {
				g_set_error_literal (error,
						     G_IO_ERROR,
						     G_IO_ERROR_CANCELLED,
						     /* TRANSLATORS: copy aborted */
						     _("Cancelled"));
				goto out;
			}
			g_set_error (error, 1, 0,
				     "Error reading %" G_GSIZE_FORMAT
				     " bytes from %" G_GUINT64_FORMAT ": ",
				     bytes_to_read,
				     bytes_completed);
			goto out;
		}
		if (bytes_read != bytes_to_read) {
			if (fill_zeros) {
				/* clear buffer to NULLs */
				memset (buffer + bytes_read, 0,
					bytes_to_read - bytes_read);
				bytes_read = bytes_to_read;
			} else {
				g_set_error (error, 1, 0,
					     "Requested %" G_GSIZE_FORMAT
					     " bytes from %" G_GUINT64_FORMAT
					     " but only read %" G_GSIZE_FORMAT " bytes",
					     bytes_to_read,
					     bytes_completed,
					     bytes_read);
				goto out;
			}
		}
		bytes_written = g_output_stream_write (device_stream,
						       buffer,
						       bytes_read,
						       cancellable,
						       &error_local);
		if (bytes_written < 0) {
			if (g_error_matches (error_local,
					     G_IO_ERROR,
					     G_IO_ERROR_CANCELLED)) {
				g_set_error_literal (error,
						     G_IO_ERROR,
						     G_IO_ERROR_CANCELLED,
						     /* TRANSLATORS: copy aborted */
						     _("Cancelled"));
				goto out;
			}
			g_set_error_literal (error, 1, 0, error_local->message);
			goto out;
		}
		bytes_completed += bytes_written;
		bytes_throughput += bytes_written;

		/* update UI */
		elapsed = g_timer_elapsed (timer, NULL);
		if (elapsed > 1.f) {
			device->throughput_w = (gdouble) bytes_throughput / elapsed;
			g_timer_reset (timer);
			bytes_throughput = 0;
		}
		device->complete = device->progress_write * (gdouble) bytes_completed /
					(gdouble) bytes_total;
		gmw_refresh_in_idle (priv);
	}

	/* success */
	ret = TRUE;
out:
	gmw_device_set_state (device, GMW_DEVICE_STATE_IDLE);
	if (device_stream != NULL)
		g_output_stream_close (G_OUTPUT_STREAM (device_stream), NULL, NULL);
	return ret;
}

/**
 * gmw_device_verify:
 **/
static gboolean
gmw_device_verify (GmwPrivate *priv,
		   GmwDevice *device,
		   GInputStream *image_stream,
		   GCancellable *cancellable,
		   GError **error)
{
	const gint buffer_size = (1 * 1024 * 1024); /* default to 1 MiB blocks */
	gboolean ret = FALSE;
	gint fd = -1;
	guchar *buffer_dest = NULL;
	guchar *buffer_src = NULL;
	guint64 bytes_completed = 0;
	guint64 bytes_throughput = 0;
	_cleanup_error_free_ GError *error_local = NULL;
	_cleanup_free_ guchar *buffer_unaligned_src = NULL;
	_cleanup_free_ guchar *buffer_unaligned_dest = NULL;
	_cleanup_object_unref_ GInputStream *device_stream = NULL;
	_cleanup_object_unref_ GUnixFDList *fd_list = NULL;
	_cleanup_timer_destroy_ GTimer *timer = NULL;
	_cleanup_variant_unref_ GVariant *fd_index = NULL;

	/* rewind */
	if (!g_seekable_seek (G_SEEKABLE (image_stream), 0,
			      G_SEEK_SET, cancellable, error))
		goto out;

	/* get fd from udisks */
	if (!udisks_block_call_open_for_backup_sync (device->udisks_block,
						     g_variant_new ("a{sv}", NULL), /* options */
						     NULL, /* fd_list */
						     &fd_index,
						     &fd_list,
						     cancellable,
						     error)) {
		if (error != NULL)
			g_dbus_error_strip_remote_error (*error);
		goto out;
	}
	fd = g_unix_fd_list_get (fd_list, g_variant_get_handle (fd_index), error);
	if (fd == -1)
		goto out;
	device_stream = g_unix_input_stream_new (fd, TRUE);

	/* read huge blocks and write it to the output device */
	buffer_unaligned_src = gmw_get_aligned_buffer (buffer_size, &buffer_src);
	buffer_unaligned_dest = gmw_get_aligned_buffer (buffer_size, &buffer_dest);
	bytes_completed = 0;
	timer = g_timer_new ();
	gmw_device_set_state (device, GMW_DEVICE_STATE_VERIFY);
	while (bytes_completed < priv->image_file_size) {
		gdouble elapsed;
		gsize bytes_to_read;
		gsize bytes_read;

		bytes_to_read = buffer_size;
		if (bytes_to_read + bytes_completed > priv->image_file_size)
			bytes_to_read = priv->image_file_size - bytes_completed;

		/* read from image */
		if (!g_input_stream_read_all (image_stream,
					      buffer_src,
					      bytes_to_read,
					      &bytes_read,
					      cancellable,
					      &error_local)) {
			if (g_error_matches (error_local,
					     G_IO_ERROR,
					     G_IO_ERROR_CANCELLED)) {
				g_set_error_literal (error,
						     G_IO_ERROR,
						     G_IO_ERROR_CANCELLED,
						     /* TRANSLATORS: copy aborted */
						     _("Cancelled"));
				goto out;
			}
			g_set_error (error, 1, 0,
				     "Error reading %" G_GSIZE_FORMAT
				     " bytes from %" G_GUINT64_FORMAT ": ",
				     bytes_to_read,
				     bytes_completed);
			goto out;
		}
		if (bytes_read != bytes_to_read) {
			g_set_error (error, 1, 0,
				     "Requested %" G_GSIZE_FORMAT
				     " bytes from %" G_GUINT64_FORMAT
				     " but only read %" G_GSIZE_FORMAT " bytes",
				     bytes_read,
				     bytes_completed,
				     bytes_to_read);
			goto out;
		}

		/* read from device */
		if (!g_input_stream_read_all (device_stream,
					      buffer_dest,
					      bytes_to_read,
					      &bytes_read,
					      cancellable,
					      &error_local)) {
			if (g_error_matches (error_local,
					     G_IO_ERROR,
					     G_IO_ERROR_CANCELLED)) {
				g_set_error_literal (error,
						     G_IO_ERROR,
						     G_IO_ERROR_CANCELLED,
						     /* TRANSLATORS: copy aborted */
						     _("Cancelled"));
				goto out;
			}
			g_set_error (error, 1, 0,
				     "Error reading %" G_GSIZE_FORMAT
				     " bytes from %" G_GUINT64_FORMAT ": ",
				     bytes_to_read,
				     bytes_completed);
			goto out;
		}
		if (bytes_read != bytes_to_read) {
			g_set_error (error, 1, 0,
				     "Requested %" G_GSIZE_FORMAT
				     " bytes from %" G_GUINT64_FORMAT
				     " but only read %" G_GSIZE_FORMAT " bytes",
				     bytes_read,
				     bytes_completed,
				     bytes_to_read);
			goto out;
		}

		/* compare */
		if (memcmp (buffer_src, buffer_dest, bytes_to_read) != 0) {
			g_set_error (error, 1, 0,
				     "Failed to verify at %" G_GUINT64_FORMAT,
				     bytes_completed);
			goto out;
		}

		bytes_completed += bytes_read;
		bytes_throughput += bytes_read;

		/* update UI */
		elapsed = g_timer_elapsed (timer, NULL);
		if (elapsed > 1.f) {
			device->throughput_r = (gdouble) bytes_throughput / elapsed;
			g_timer_reset (timer);
			bytes_throughput = 0;
		}

		/* update UI */
		device->complete = device->progress_write +
			(1.0 - device->progress_write) *
				((gdouble) bytes_completed /
					(gdouble) priv->image_file_size);
		gmw_refresh_in_idle (priv);
	}

	/* success */
	ret = TRUE;
out:
	gmw_device_set_state (device, GMW_DEVICE_STATE_IDLE);
	if (device_stream != NULL)
		g_input_stream_close (G_INPUT_STREAM (device_stream), NULL, NULL);
	return ret;
}

/**
 * gmw_refresh_titlebar:
 **/
static void
gmw_refresh_titlebar (GmwPrivate *priv)
{
	GmwDevice *device;
	GtkWidget *w;
	gdouble throughput_r = 0.f;
	gdouble throughput_w = 0.f;
	guint i;
	_cleanup_string_free_ GString *title = NULL;

	/* find the throughput totals */
	g_mutex_lock (&priv->devices_mutex);
	for (i = 0; i < priv->devices->len; i++) {
		device = g_ptr_array_index (priv->devices, i);
		g_mutex_lock (&device->mutex);
		throughput_r += device->throughput_r;
		throughput_w += device->throughput_w;
		g_mutex_unlock (&device->mutex);
	}
	g_mutex_unlock (&priv->devices_mutex);

	/* set the title */
	title = g_string_new (_("MultiWriter"));
	if (throughput_w > 0.f) {
		g_string_append_printf (title, " → %.0f MB/s",
					throughput_w / (1000 * 1000));
	}
	if (throughput_r > 0.f) {
		_cleanup_free_ gchar *tmp = NULL;
		tmp = g_strdup_printf ("%.0f MB/s → ",
				       throughput_r / (1000 * 1000));
		g_string_prepend (title, tmp);
	}
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "header"));
	gtk_header_bar_set_title (GTK_HEADER_BAR (w), title->str);
}

/**
 * gmw_refresh_titlebar_idle_cb:
 **/
static gboolean
gmw_refresh_titlebar_idle_cb (gpointer user_data)
{
	GmwPrivate *priv = (GmwPrivate *) user_data;
	gmw_refresh_titlebar (priv);
	return G_SOURCE_REMOVE;
}

/**
 * gmw_copy_thread_cb:
 **/
static void
gmw_copy_thread_cb (gpointer data, gpointer user_data)
{
	GmwDevice *device = (GmwDevice *) data;
	GmwPrivate *priv = (GmwPrivate *) user_data;
	_cleanup_error_free_ GError *error = NULL;
	_cleanup_object_unref_ GInputStream *image_stream = NULL;

	/* set the factor for the write process */
	device->progress_write = 0.75;
	if (!g_settings_get_boolean (priv->settings, "enable-verify"))
		device->progress_write = 1.0;

	/* open input stream */
	image_stream = (GInputStream *) g_file_read (priv->image_file, NULL, &error);
	if (image_stream == NULL) {
		gmw_device_set_error (device, error);
		goto out;
	}

	/* write */
	if (!gmw_device_write (priv, device, image_stream,
			       priv->cancellable, &error)) {
		gmw_device_set_error (device, error);
		goto out;
	}

	/* verify */
	if (g_settings_get_boolean (priv->settings, "enable-verify")) {
		if (!gmw_device_verify (priv, device, image_stream,
					priv->cancellable, &error)) {
			gmw_device_set_error (device, error);
			goto out;
		}
	}

	/* no longer show progressbar */
	gmw_device_set_state (device, GMW_DEVICE_STATE_SUCCESS);
out:
	gmw_refresh_in_idle (priv);
	g_idle_add (gmw_refresh_titlebar_idle_cb, priv);
	g_timeout_add_seconds (2, gmw_refresh_in_idle_cb, priv);
	g_input_stream_close (G_INPUT_STREAM (image_stream), NULL, NULL);
	gmw_copy_done (priv);
}

/**
 * gmw_update_title:
 **/
static void
gmw_update_title (GmwPrivate *priv)
{
	GtkWidget *w;
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "header"));
	if (priv->image_file == NULL) {
		gtk_header_bar_set_subtitle (GTK_HEADER_BAR (w), NULL);
	} else {
		_cleanup_free_ gchar *basename = NULL;
		basename = g_file_get_basename (priv->image_file);
		gtk_header_bar_set_subtitle (GTK_HEADER_BAR (w), basename);
	}
}

/**
 * gmw_set_image_filename:
 **/
static void
gmw_set_image_filename (GmwPrivate *priv, const gchar *filename)
{
	_cleanup_error_free_ GError *error = NULL;
	_cleanup_object_unref_ GFileInfo *info = NULL;

	if (priv->image_file != NULL)
		g_object_unref (priv->image_file);
	priv->image_file = g_file_new_for_path (filename);

	/* get the size of the ISO file */
	info = g_file_query_info (priv->image_file,
				  G_FILE_ATTRIBUTE_STANDARD_SIZE,
				  G_FILE_QUERY_INFO_NONE,
				  NULL,
				  &error);
	if (info == NULL) {
		/* TRANSLATORS: we couldn't open the ISO file the user chose */
		gmw_error_dialog (priv, _("Failed to open"), error->message);
		return;
	}
	priv->image_file_size = g_file_info_get_size (info);
}

/**
 * gmw_import_filename:
 **/
static void
gmw_import_filename (GmwPrivate *priv)
{
	GtkFileFilter *filter = NULL;
	GtkWidget *d;
	GtkWidget *w;
	_cleanup_error_free_ GError *error = NULL;
	_cleanup_object_unref_ GFile *file = NULL;

	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_main"));
	/* TRANSLATORS: window title for the file-chooser, file is an ISO */
	d = gtk_file_chooser_dialog_new (_("Choose the file to write"),
					 GTK_WINDOW (w),
					 GTK_FILE_CHOOSER_ACTION_OPEN,
					 /* TRANSLATORS: button title */
					 _("Cancel"), GTK_RESPONSE_CANCEL,
					 /* TRANSLATORS: button title */
					 _("Import"), GTK_RESPONSE_ACCEPT,
					 NULL);
	filter = gtk_file_filter_new ();
	/* TRANSLATORS: the file filter description, e.g. *.iso */
	gtk_file_filter_set_name (filter, _("ISO files"));
	gtk_file_filter_add_pattern (filter, "*.iso");
	gtk_file_filter_add_pattern (filter, "*.img");
	gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (d), filter);
	if (gtk_dialog_run (GTK_DIALOG (d)) == GTK_RESPONSE_ACCEPT) {
		_cleanup_free_ gchar *filename = NULL;
		filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (d));
		gmw_set_image_filename (priv, filename);
		gmw_update_title (priv);
		g_settings_set_string (priv->settings, "filename", filename);
	}
	gtk_widget_destroy (d);
}

/**
 * gmw_auth_dummy_restore:
 **/
static gboolean
gmw_auth_dummy_restore (GmwPrivate *priv, GmwDevice *device, GError **error)
{
	gboolean ret = FALSE;
	gint fd = -1;
	_cleanup_object_unref_ GUnixFDList *fd_list = NULL;
	_cleanup_variant_unref_ GVariant *fd_index = NULL;

	if (!udisks_block_call_open_for_restore_sync (device->udisks_block,
						      g_variant_new ("a{sv}", NULL), /* options */
						      NULL, /* fd_list */
						      &fd_index,
						      &fd_list,
						      priv->cancellable,
						      error)) {
		goto out;
	}
	fd = g_unix_fd_list_get (fd_list, g_variant_get_handle (fd_index), error);
	if (fd == -1)
		goto out;

	/* success */
	ret = TRUE;
out:
	if (fd != -1)
		g_close (fd, NULL);
	return ret;
}

/**
 * gmw_throughput_update_titlebar_cb:
 **/
static gboolean
gmw_throughput_update_titlebar_cb (gpointer user_data)
{
	GmwPrivate *priv = (GmwPrivate *) user_data;
	gmw_refresh_titlebar (priv);
	return G_SOURCE_CONTINUE;
}

/**
 * gmw_udisks_unmount_cb:
 **/
static void
gmw_udisks_unmount_cb (GObject *source_object,
		       GAsyncResult *res,
		       gpointer user_data)
{
	UDisksFilesystem *udisks_fs = UDISKS_FILESYSTEM (source_object);
	_cleanup_error_free_ GError *error = NULL;

	if (!udisks_filesystem_call_unmount_finish (udisks_fs, res, &error))
		g_warning ("Failed to unmount filesystem: %s", error->message);
}

/**
 * gmw_udisks_unmount_filesystems:
 **/
static void
gmw_udisks_unmount_filesystems (GmwPrivate *priv, GmwDevice *device, gboolean sync)
{
	_cleanup_error_free_ GError *error = NULL;
	_cleanup_free_ gchar *object_path_child = NULL;
	_cleanup_object_unref_ UDisksBlock *udisks_block = NULL;
	_cleanup_object_unref_ UDisksObject *udisks_object = NULL;
	_cleanup_object_unref_ UDisksFilesystem *udisks_fs = NULL;
	_cleanup_strv_free_ gchar **mtab = NULL;

	object_path_child = g_strdup_printf ("%s1", device->object_path);
	udisks_object = udisks_client_get_object (priv->udisks_client,
						  object_path_child);
	if (udisks_object == NULL)
		return;
	udisks_fs = udisks_object_get_filesystem (udisks_object);
	if (udisks_fs == NULL)
		return;
	mtab = udisks_filesystem_dup_mount_points (udisks_fs);
	if (mtab == NULL || mtab[0] == NULL) {
		g_debug ("%s not mounted", object_path_child);
		return;
	}
	g_debug ("Unmount %s from %s", mtab[0], object_path_child);
	if (!sync) {
		udisks_filesystem_call_unmount (udisks_fs,
						g_variant_new ("a{sv}", NULL),
						priv->cancellable,
						gmw_udisks_unmount_cb,
						device);
		return;
	}
	if (!udisks_filesystem_call_unmount_sync (udisks_fs,
						  g_variant_new ("a{sv}", NULL),
						  priv->cancellable,
						  &error)) {
		g_warning ("Failed to unmount fs: %s", error->message);
	}
}

/**
 * gmw_start_copy:
 **/
static void
gmw_start_copy (GmwPrivate *priv)
{
	GmwDevice *device;
	GtkWindow *window;
	guint i;
	_cleanup_error_free_ GError *error = NULL;

	/* if nothing already set, request this now */
	if (priv->image_file == NULL)
		gmw_import_filename (priv);
	if (priv->image_file == NULL)
		return;

	g_cancellable_reset (priv->cancellable);

	/* unmount all filesystems */
	for (i = 0; i < priv->devices->len; i++) {
		device = g_ptr_array_index (priv->devices, i);
		gmw_udisks_unmount_filesystems (priv, device, TRUE);
	}

	/* do a dummy call to get the PolicyKit auth */
	if (!priv->done_polkit_auth) {
		device = g_ptr_array_index (priv->devices, 0);
		if (!gmw_auth_dummy_restore (priv, device, &error)) {
			g_dbus_error_strip_remote_error (error);
			gmw_error_dialog (priv,
					  /* TRANSLATORS: error dialog title:
					   * we probably didn't authenticate */
					  _("Failed to copy"),
					  error->message);
			return;
		}
		priv->done_polkit_auth = TRUE;
	}

	/* update the global stats */
	priv->throughput_id =
		g_timeout_add_seconds (1, gmw_throughput_update_titlebar_cb, priv);

	/* don't allow suspend */
	window = GTK_WINDOW (gtk_builder_get_object (priv->builder, "dialog_main"));
	priv->inhibit_id = gtk_application_inhibit (priv->application,
						    window,
						    GTK_APPLICATION_INHIBIT_SUSPEND |
						    GTK_APPLICATION_INHIBIT_LOGOUT,
						    /* TRANSLATORS: the inhibit reason */
						    _("Writing ISO to devices"));

	/* start a thread for each copy operation */
	for (i = 0; i < priv->devices->len; i++) {
		_cleanup_free_ gchar *title = NULL;
		device = g_ptr_array_index (priv->devices, i);
		gmw_device_set_state (device, GMW_DEVICE_STATE_WAITING);
		g_thread_pool_push (priv->thread_pool, device, &error);
	}
	gmw_refresh_ui (priv);
}

/**
 * gmw_start_confirm_response_cb:
 **/
static void
gmw_start_confirm_response_cb (GtkDialog *dialog,
			       GtkResponseType response_id,
			       GmwPrivate *priv)
{
	if (response_id == GTK_RESPONSE_OK) {
		g_settings_set_boolean (priv->settings, "show-warning", FALSE);
		gmw_start_copy (priv);
	}
	gtk_widget_destroy (GTK_WIDGET (dialog));
}

/**
 * gmw_start_clicked_cb:
 **/
static void
gmw_start_clicked_cb (GtkWidget *widget, GmwPrivate *priv)
{
	GtkWidget *w;
	GtkWidget *b;
	GtkStyleContext *context;
	_cleanup_string_free_ GString *str = g_string_new ("");

	/* no confirmation required */
	if (!g_settings_get_boolean (priv->settings, "show-warning")) {
		gmw_start_copy (priv);
		return;
	}

	/* create warning dialog */
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_main"));
	w = gtk_message_dialog_new (GTK_WINDOW (w),
				    GTK_DIALOG_DESTROY_WITH_PARENT |
				    GTK_DIALOG_MODAL |
				    GTK_DIALOG_USE_HEADER_BAR,
				    GTK_MESSAGE_WARNING,
				    GTK_BUTTONS_CANCEL,
				    /* TRANSLATORS: window title for the warning dialog */
				    "%s", _("Write to all disks?"));

	/* use different message text for each setting */
	if (g_settings_get_boolean (priv->settings, "blank-drive")) {
		/* TRANSLATORS: check that we can nuke everything from all disks */
		g_string_append (str, _("All data on the drives will be deleted."));
	} else {
		/* TRANSLATORS: if the image file is smaller than the disks and
		 * we've disabled wiping the device we only write enough data
		 * to transfer the image */
		g_string_append (str, _("The ISO file is smaller than the "
					"disk capacity."));
		g_string_append (str, " ");

		/* TRANSLATORS: this could leave your personal files on the drive */
		g_string_append (str, _("Some of the current contents of the "
					"drives could be still found using "
					"forensic tools even after copying."));
	}
	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (w), "%s", str->str);

	/* TRANSLATORS: button text for the warning dialog */
	b = gtk_dialog_add_button (GTK_DIALOG (w), _("I Understand"), GTK_RESPONSE_OK);
	context = gtk_widget_get_style_context (b);
	gtk_style_context_add_class (context, GTK_STYLE_CLASS_SUGGESTED_ACTION);
	g_signal_connect (w, "response",
			  G_CALLBACK (gmw_start_confirm_response_cb), priv);
	gtk_window_present (GTK_WINDOW (w));
}

/**
 * gmw_import_activated_cb:
 **/
static void
gmw_import_activated_cb (GSimpleAction *action,
			 GVariant *parameter,
			 gpointer user_data)
{
	GmwPrivate *priv = (GmwPrivate *) user_data;
	gmw_import_filename (priv);
}

/**
 * gmw_about_activated_cb:
 **/
static void
gmw_about_activated_cb (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	GmwPrivate *priv = (GmwPrivate *) user_data;
	GList *windows;
	GtkIconTheme *icon_theme;
	GtkWindow *parent = NULL;
	const gchar *authors[] = { "Richard Hughes", NULL };
	const gchar *copyright = "Copyright \xc2\xa9 2014-2015 Richard Hughes";
	_cleanup_object_unref_ GdkPixbuf *logo = NULL;

	windows = gtk_application_get_windows (GTK_APPLICATION (priv->application));
	if (windows)
		parent = windows->data;

	icon_theme = gtk_icon_theme_get_default ();
	logo = gtk_icon_theme_load_icon (icon_theme, "drive-harddisk-usb", 256, 0, NULL);
	gtk_show_about_dialog (parent,
			       /* TRANSLATORS: the title of the about window */
			       "title", _("About GNOME MultiWriter"),
			       /* TRANSLATORS: the application name for the about UI */
			       "program-name", _("GNOME MultiWriter"),
			       "authors", authors,
			       /* TRANSLATORS: one-line description for the app */
			       "comments", _("Write an ISO file to multiple USB devices at once"),
			       "copyright", copyright,
			       "license-type", GTK_LICENSE_GPL_2_0,
			       "logo", logo,
			       /* TRANSLATORS: you can put your name here :) */
			       "translator-credits", _("translator-credits"),
			       "version", VERSION,
			       NULL);
}

/**
 * gmw_quit_activated_cb:
 **/
static void
gmw_quit_activated_cb (GSimpleAction *action,
		       GVariant *parameter,
		       gpointer user_data)
{
	GmwPrivate *priv = (GmwPrivate *) user_data;
	g_application_quit (G_APPLICATION (priv->application));
}

static GActionEntry actions[] = {
	{ "about", gmw_about_activated_cb, NULL, NULL, NULL },
	{ "import", gmw_import_activated_cb, NULL, NULL, NULL },
	{ "quit", gmw_quit_activated_cb, NULL, NULL, NULL }
};

/**
 * gmw_settings_clicked_cb:
 **/
static void
gmw_settings_clicked_cb (GtkWidget *widget, GmwPrivate *priv)
{
	GtkWidget *pop;
	GtkWidget *box;

	/* reclaim */
	if (gtk_widget_get_parent (priv->switch_verify) != NULL)
		g_object_ref (priv->switch_verify);
	gtk_widget_unparent (priv->switch_verify);
	if (gtk_widget_get_parent (priv->switch_blank) != NULL)
		g_object_ref (priv->switch_blank);
	gtk_widget_unparent (priv->switch_blank);

	/* show settings */
	pop = gtk_popover_new (widget);
	gtk_popover_set_position (GTK_POPOVER (pop), GTK_POS_BOTTOM);
	gtk_container_set_border_width (GTK_CONTAINER (pop), 18);
	box = gtk_grid_new ();
	gtk_grid_set_row_spacing (GTK_GRID (box), 6);
	gtk_grid_set_column_spacing (GTK_GRID (box), 12);
	gtk_grid_attach (GTK_GRID (box),
			 /* TRANSLATORS: a switch label: verify the image by
			  * reading back the original image from the device */
			 gtk_label_new (_("Verify")),
			 0, 0, 1, 1);
	gtk_grid_attach (GTK_GRID (box), priv->switch_verify, 1, 0, 1, 1);
	gtk_grid_attach (GTK_GRID (box),
			 /* TRANSLATORS: a switch label: we write zeros after
			  * the image so it erases the entire device */
			 gtk_label_new (_("Wipe")),
			 0, 1, 1, 1);
	gtk_grid_attach (GTK_GRID (box), priv->switch_blank, 1, 1, 1, 1);
	gtk_container_add (GTK_CONTAINER (pop), box);
	gtk_widget_show_all (pop);
}

/**
 * gmw_startup_cb:
 **/
static void
gmw_startup_cb (GApplication *application, GmwPrivate *priv)
{
	GtkWidget *main_window;
	GtkWidget *w;
	gint retval;
	_cleanup_error_free_ GError *error = NULL;
	_cleanup_free_ gchar *filename = NULL;
	_cleanup_object_unref_ GdkPixbuf *pixbuf = NULL;

	/* add application menu items */
	g_action_map_add_action_entries (G_ACTION_MAP (application),
					 actions, G_N_ELEMENTS (actions),
					 priv);

	/* get UI */
	priv->builder = gtk_builder_new ();
	retval = gtk_builder_add_from_resource (priv->builder,
						"/org/gnome/MultiWriter/gmw-main.ui",
						&error);
	if (retval == 0) {
		g_warning ("failed to load ui: %s", error->message);
		return;
	}

	main_window = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_main"));
	gtk_application_add_window (priv->application, GTK_WINDOW (main_window));
	gtk_widget_set_size_request (main_window, 750, 200);

	/* Hide window first so that the dialogue resizes itself without redrawing */
	gtk_widget_hide (main_window);

	/* buttons */
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_settings"));
	g_signal_connect (w, "clicked",
			  G_CALLBACK (gmw_settings_clicked_cb), priv);
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_start"));
	g_signal_connect (w, "clicked",
			  G_CALLBACK (gmw_start_clicked_cb), priv);
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_cancel"));
	g_signal_connect (w, "clicked",
			  G_CALLBACK (gmw_cancel_clicked_cb), priv);

	/* setup USB image */
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "image_usb"));
	pixbuf = gdk_pixbuf_new_from_resource_at_scale ("/org/gnome/MultiWriter/usb.svg",
							-1, 48, TRUE, &error);
	if (pixbuf == NULL) {
		g_warning ("failed to load usb.svg: %s", error->message);
		return;
	}
	gtk_image_set_from_pixbuf (GTK_IMAGE (w), pixbuf);

	/* get previously loaded image */
	filename = g_settings_get_string (priv->settings, "filename");
	if (filename != NULL && filename[0] != '\0')
		gmw_set_image_filename (priv, filename);

	/* show main UI */
	gmw_update_title (priv);
	gmw_refresh_ui (priv);
	gtk_widget_show (main_window);
}

/**
 * gmw_sysfs_get_busnum:
 **/
static guint8
gmw_sysfs_get_busnum (const gchar *filename)
{
	_cleanup_free_ gchar *data = NULL;
	_cleanup_free_ gchar *path = NULL;
	path = g_build_filename (filename, "busnum", NULL);
	if (!g_file_get_contents (path, &data, NULL, NULL))
		return 0;
	return g_ascii_strtoull (data, NULL, 10);
}

/**
 * gmw_sysfs_get_devnum:
 **/
static guint8
gmw_sysfs_get_devnum (const gchar *filename)
{
	_cleanup_free_ gchar *data = NULL;
	_cleanup_free_ gchar *path = NULL;
	path = g_build_filename (filename, "devnum", NULL);
	if (!g_file_get_contents (path, &data, NULL, NULL))
		return 0;
	return g_ascii_strtoull (data, NULL, 10);
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
	const gchar	*platform_id;		/* new label */
} GmwQuirk;
#endif

/**
 * gmw_udisks_get_quirk_id:
 **/
static gchar *
gmw_udisks_get_quirk_id (GmwPrivate *priv, GUsbDevice *usb_device)
{
#if G_USB_CHECK_VERSION(0,2,4)
	guint i;
	guint j;
	_cleanup_object_unref_ GUsbDevice *usb_hub = NULL;
	_cleanup_object_unref_ GUsbDevice *usb_hub_parent = NULL;
	const GmwQuirk quirks[] = {
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

	for (i = 0; quirks[i].platform_id != NULL; i++) {
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

		/* get the top-level port address prepended to the decal name */
		if (usb_hub_parent != NULL && quirks[i].chain_len == 1) {
			return g_strdup_printf ("%02x:%02x [%s]",
						g_usb_device_get_bus (usb_hub_parent),
						g_usb_device_get_address (usb_hub_parent),
						quirks[i].platform_id);
		}
		return g_strdup_printf ("%02x:%02x [%s]",
					g_usb_device_get_bus (usb_hub),
					g_usb_device_get_address (usb_hub),
					quirks[i].platform_id);
	}
#endif
	return NULL;
}

/**
 * gmw_udisks_get_sibling_id:
 **/
static gchar *
gmw_udisks_get_sibling_id (GmwPrivate *priv, UDisksDrive *udisks_drive)
{
	const gchar *sibling_id;
	gchar *parent_id = NULL;
	gchar *quirk_id;
	guint8 busnum;
	guint8 devnum;
	_cleanup_object_unref_ GUsbDevice *usb_device = NULL;
	_cleanup_strv_free_ gchar **split = NULL;

	/* get the sibling ID, which is normally the USB path */
	sibling_id = udisks_drive_get_sibling_id (udisks_drive);
	if (sibling_id == NULL || sibling_id[0] == '\0')
		return g_strdup ("???");

	/* find the USB device using GUsb */
	parent_id = g_path_get_dirname (sibling_id);
	busnum = gmw_sysfs_get_busnum (parent_id);
	devnum = gmw_sysfs_get_devnum (parent_id);
	if (busnum == 0x00 || devnum == 0x00) {
		g_warning ("Failed to get busnum for %s", parent_id);
		return g_path_get_basename (parent_id);
	}
	usb_device = g_usb_context_find_by_bus_address (priv->usb_ctx,
							busnum,
							devnum,
							NULL);
	if (usb_device == NULL) {
		g_warning ("Failed to find %02x:%02x", busnum, devnum);
		return g_path_get_basename (parent_id);
	}

	/* can we get the ID from a quirk */
	quirk_id = gmw_udisks_get_quirk_id (priv, usb_device);
	if (quirk_id != NULL)
		return quirk_id;

	/* return the bare platform ID */
	return g_strdup (g_usb_device_get_platform_id (usb_device) + 7);
}

/**
 * gmw_udisks_object_add:
 **/
static void
gmw_udisks_object_add (GmwPrivate *priv, GDBusObject *dbus_object)
{
	GmwDevice *device;
	const gchar *device_path;
	const gchar *object_path;
	guint64 device_size;
	_cleanup_object_unref_ GDBusInterface *iface_block = NULL;
	_cleanup_object_unref_ GDBusInterface *iface_fs = NULL;
	_cleanup_object_unref_ GDBusInterface *iface_part = NULL;
	_cleanup_object_unref_ UDisksBlock *udisks_block = NULL;
	_cleanup_object_unref_ UDisksDrive *udisks_drive = NULL;
	_cleanup_object_unref_ UDisksObjectInfo *object_info = NULL;
	_cleanup_object_unref_ UDisksObject *udisks_object = NULL;

	/* is this the kind of device that interests us? */
	iface_block = g_dbus_object_get_interface (dbus_object, "org.freedesktop.UDisks2.Block");
	if (iface_block == NULL)
		return;
	iface_part = g_dbus_object_get_interface (dbus_object, "org.freedesktop.UDisks2.Partition");
	if (iface_part != NULL)
		return;
	iface_fs = g_dbus_object_get_interface (dbus_object, "org.freedesktop.UDisks2.Filesystem");
	if (iface_fs != NULL)
		return;

	/* get the block device */
	object_path = g_dbus_object_get_object_path (dbus_object);
	udisks_object = udisks_client_get_object (priv->udisks_client, object_path);
	udisks_block = udisks_object_get_block (udisks_object);
	if (udisks_block == NULL)
		return;

	/* ignore system devices */
	device_path = udisks_block_get_device (udisks_block);
	if (udisks_block_get_hint_system (udisks_block)) {
		g_debug ("%s is a system device", device_path);
		return;
	}

	/* ignore small or large devices */
	device_size = udisks_block_get_size (udisks_block) / (1000 * 1000);
	if (device_size < 1000) {
		g_debug ("%s is too small [%u]",
			 device_path, (guint) device_size);
		return;
	}
	if (device_size > 1000 * 16) {
		g_debug ("%s is too large [%u]",
			 device_path, (guint) device_size);
		return;
	}

	/* get the drive */
	udisks_drive = udisks_client_get_drive_for_block (priv->udisks_client, udisks_block);
	if (udisks_drive == NULL) {
		g_debug ("%s has no drive", device_path);
		return;
	}

	/* add this */
	object_info = udisks_client_get_object_info (priv->udisks_client, udisks_object);
	device = gmw_device_new ();
	device->device_name = g_strdup (udisks_object_info_get_name (object_info));
	device->device_path = g_strdup (device_path);
	device->object_path = g_strdup (object_path);
	device->udisks_block = g_object_ref (udisks_block);
	device->sibling_id = gmw_udisks_get_sibling_id (priv, udisks_drive);
	g_mutex_init (&device->mutex);

	/* unmount filesystems on the block device */
	gmw_udisks_unmount_filesystems (priv, device, FALSE);

	g_mutex_lock (&priv->devices_mutex);
	g_ptr_array_add (priv->devices, device);
	g_ptr_array_sort (priv->devices, gmw_devices_sort_cb);
	g_mutex_unlock (&priv->devices_mutex);
	g_debug ("Added %s [%lu]", device_path, device_size);
}

/**
 * gmw_udisks_object_added_cb:
 **/
static void
gmw_udisks_object_added_cb (GDBusObjectManager *object_manager,
			    GDBusObject *dbus_object,
			    GmwPrivate *priv)
{
	gmw_udisks_object_add (priv, dbus_object);
	gmw_refresh_ui (priv);
}

/**
 * gmw_udisks_object_removed_cb:
 **/
static void
gmw_udisks_object_removed_cb (GDBusObjectManager *object_manager,
			      GDBusObject *dbus_object,
			      GmwPrivate *priv)
{
	GmwDevice *device;
	guint i;
	const gchar *tmp;

	/* remove device */
	tmp = g_dbus_object_get_object_path (dbus_object);
	for (i = 0; i < priv->devices->len; i++) {
		device = g_ptr_array_index (priv->devices, i);
		if (g_strcmp0 (device->object_path, tmp) == 0) {
			g_ptr_array_remove (priv->devices, device);
			g_ptr_array_sort (priv->devices, gmw_devices_sort_cb);
			gmw_refresh_ui (priv);
			break;
		}
	}
}

/**
 * gmw_udisks_client_connect_cb:
 **/
static void
gmw_udisks_client_connect_cb (GObject *source_object,
			      GAsyncResult *res,
			      gpointer user_data)
{
	GmwPrivate *priv = (GmwPrivate *) user_data;
	GDBusObjectManager *object_manager;
	GList *l;
	GList *objects;
	_cleanup_error_free_ GError *error = NULL;

	priv->udisks_client = udisks_client_new_finish (res, &error);
	if (priv->udisks_client == NULL) {
		g_warning ("Failed to contact UDisks: %s", error->message);
		return;
	}
	object_manager = udisks_client_get_object_manager (priv->udisks_client);
	g_signal_connect (object_manager, "object-added",
			  G_CALLBACK (gmw_udisks_object_added_cb), priv);
	g_signal_connect (object_manager, "object-removed",
			  G_CALLBACK (gmw_udisks_object_removed_cb), priv);
	objects = g_dbus_object_manager_get_objects (object_manager);
	for (l = objects; l != NULL; l = l->next)
		gmw_udisks_object_add (priv, G_DBUS_OBJECT (l->data));
	gmw_refresh_ui (priv);
	g_list_free_full (objects, (GDestroyNotify) g_object_unref);
}

/**
 * gmw_settings_changed_cb:
 **/
static void
gmw_settings_changed_cb (GSettings *settings, const gchar *key, GmwPrivate *priv)
{
	if (g_strcmp0 (key, "blank-drive") == 0) {
		g_settings_set_boolean (settings, "show-warning", TRUE);
		return;
	}
	if (g_strcmp0 (key, "max-threads") == 0) {
		g_thread_pool_set_max_threads (priv->thread_pool,
					       g_settings_get_uint (settings, key),
					       NULL);
		return;
	}
}

/**
 * gmw_thread_pool_sort_func:
 **/
static gint
gmw_thread_pool_sort_func (gconstpointer a, gconstpointer b, gpointer user_data)
{
	GmwDevice *device_a = (GmwDevice *) a;
	GmwDevice *device_b = (GmwDevice *) b;
	return g_strcmp0 (device_a->sibling_id, device_b->sibling_id);
}

/**
 * main:
 **/
int
main (int argc, char **argv)
{
	GmwPrivate *priv = NULL;
	GOptionContext *context;
	gboolean verbose = FALSE;
	int status = EXIT_SUCCESS;
	_cleanup_error_free_ GError *error = NULL;
	const GOptionEntry options[] = {
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
			/* TRANSLATORS: command line option */
			_("Show extra debugging information"), NULL },
		{ NULL}
	};

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	gtk_init (&argc, &argv);

	/* TRANSLATORS: A program to copy the LiveUSB image onto USB hardware */
	context = g_option_context_new (_("GNOME MultiWriter"));
	g_option_context_add_group (context, gtk_get_option_group (TRUE));
	g_option_context_add_main_entries (context, options, NULL);
	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		status = EXIT_FAILURE;
		/* TRANSLATORS: the user has sausages for fingers */
		g_print ("%s: %s\n", _("Failed to parse command line options"),
			 error->message);
		goto out;
	}

	priv = g_new0 (GmwPrivate, 1);
	g_mutex_init (&priv->thread_pool_mutex);
	g_mutex_init (&priv->devices_mutex);
	g_mutex_init (&priv->idle_id_mutex);
	priv->cancellable = g_cancellable_new ();
	priv->settings = g_settings_new ("org.gnome.MultiWriter");
	g_signal_connect (priv->settings, "changed",
			  G_CALLBACK (gmw_settings_changed_cb), priv);
	priv->thread_pool = g_thread_pool_new (gmw_copy_thread_cb, priv,
					       g_settings_get_uint (priv->settings, "max-threads"),
					       FALSE, &error);
	if (priv->thread_pool == NULL) {
		status = EXIT_FAILURE;
		g_print ("Failed to create thread pool: %s\n", error->message);
		goto out;
	}
	g_thread_pool_set_sort_function (priv->thread_pool,
					 gmw_thread_pool_sort_func,
					 priv);
	priv->devices = g_ptr_array_new_with_free_func ((GDestroyNotify) gmw_device_free);
	priv->usb_ctx = g_usb_context_new (NULL);

	/* connect to UDisks */
	udisks_client_new (NULL, gmw_udisks_client_connect_cb, priv);

	/* ensure single instance */
	priv->application = gtk_application_new ("org.gnome.MultiWriter", 0);
	g_signal_connect (priv->application, "startup",
			  G_CALLBACK (gmw_startup_cb), priv);
	g_signal_connect (priv->application, "activate",
			  G_CALLBACK (gmw_activate_cb), priv);
	/* set verbose? */
	if (verbose)
		g_setenv ("G_MESSAGES_DEBUG", "GnomeMultiWriter", FALSE);

	/* keep these local as they get reparented to the popover */
	priv->switch_verify = gtk_switch_new ();
	priv->switch_blank = gtk_switch_new ();
	g_settings_bind (priv->settings, "enable-verify",
			 priv->switch_verify, "active",
			 G_SETTINGS_BIND_DEFAULT);
	g_settings_bind (priv->settings, "blank-drive",
			 priv->switch_blank, "active",
			 G_SETTINGS_BIND_DEFAULT);

	/* wait */
	status = g_application_run (G_APPLICATION (priv->application), argc, argv);
out:
	g_option_context_free (context);
	if (priv != NULL) {
		if (priv->builder != NULL)
			g_object_unref (priv->builder);
		if (priv->settings != NULL)
			g_object_unref (priv->settings);
		if (priv->udisks_client != NULL)
			g_object_unref (priv->udisks_client);
		if (priv->usb_ctx != NULL)
			g_object_unref (priv->usb_ctx);
		if (priv->image_file != NULL)
			g_object_unref (priv->image_file);
		if (priv->cancellable != NULL)
			g_object_unref (priv->cancellable);
		g_object_unref (priv->application);
		g_thread_pool_free (priv->thread_pool, TRUE, TRUE);
		g_mutex_clear (&priv->thread_pool_mutex);
		g_mutex_clear (&priv->devices_mutex);
		g_mutex_clear (&priv->idle_id_mutex);
		g_ptr_array_unref (priv->devices);
		g_free (priv);
	}
	return status;
}
