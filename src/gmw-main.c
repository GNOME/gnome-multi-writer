/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2014-2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
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
#include <polkit/polkit.h>

#include "gmw-cleanup.h"
#include "gmw-device.h"

typedef struct {
	guint			 idx;
	GPtrArray		*devices;
} GmwRootHub;

typedef struct {
	GFile			*image_file;
	GFileMonitor		*image_monitor;
	guint64			 image_file_size;
	gboolean		 rename_labels;
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
	guint			 inhibit_id;
	guint			 throughput_id;
	guint			 idle_id;
	GMutex			 idle_id_mutex;
	GtkWidget		*switch_verify;
	GtkWidget		*switch_blank;
	GtkWidget		*switch_probe;
} GmwPrivate;


static void	 gmw_import_filename	(GmwPrivate	*priv);

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

static void
gmw_activate_cb (GApplication *application, GmwPrivate *priv)
{
	GtkWindow *window;
	window = GTK_WINDOW (gtk_builder_get_object (priv->builder, "dialog_main"));
	gtk_window_present (window);
}

static void
gmw_cancel_clicked_cb (GtkWidget *widget, GmwPrivate *priv)
{
	g_cancellable_cancel (priv->cancellable);
}

static void
gmw_import_clicked_cb (GtkWidget *widget, GmwPrivate *priv)
{
	gmw_import_filename (priv);
}

static gint
gmw_devices_sort_cb (gconstpointer a, gconstpointer b)
{
	GmwDevice *deva = *((GmwDevice **) a);
	GmwDevice *devb = *((GmwDevice **) b);
	return g_strcmp0 (gmw_device_get_order_display (deva),
			  gmw_device_get_order_display (devb));
}

static void
gmw_root_hub_free (GmwRootHub *rh)
{
	g_ptr_array_unref (rh->devices);
	g_free (rh);
}

static GmwRootHub *
gmw_root_hub_new (guint idx)
{
	GmwRootHub *rh;
	rh = g_new0 (GmwRootHub, 1);
	rh->idx = idx;
	rh->devices = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	return rh;
}

static GmwRootHub *
gmw_root_hub_find_by_idx (GPtrArray *root_hubs, guint idx)
{
	for (guint i = 0; i < root_hubs->len; i++) {
		GmwRootHub *rh = g_ptr_array_index (root_hubs, i);
		if (rh->idx == idx)
			return rh;
	}
	return NULL;
}

/**
 * gmw_root_hub_enumerate:
 * @devices: (element-type GmwDevice): connected devices
 *
 * Gets a list of all the root hubs in use.
 *
 * Returns: (transfer full) (element-type GmwRootHub): root hubs
 **/
static GPtrArray *
gmw_root_hub_enumerate (GPtrArray *devices)
{
	GPtrArray *array;

	/* put each device into an array of its root hub */
	array = g_ptr_array_new_with_free_func ((GDestroyNotify) gmw_root_hub_free);
	for (guint i = 0; i < devices->len; i++) {
		GmwDevice *device = g_ptr_array_index (devices, i);
		guint idx = gmw_device_get_hub_root (device);
		GmwRootHub *rh = gmw_root_hub_find_by_idx (array, idx);
		if (rh == NULL) {
			rh = gmw_root_hub_new (idx);
			g_ptr_array_add (array, rh);
		}
		g_ptr_array_add (rh->devices, g_object_ref (device));
	}
	return array;
}

static void
gmw_device_list_sort (GmwPrivate *priv)
{
	GmwDevice *device;
	guint idx = 0;
	g_autoptr(GPtrArray) root_hubs = NULL;

	/* first, sort the list for display */
	g_ptr_array_sort (priv->devices, gmw_devices_sort_cb);

	/* queue the devices according to the root hub they are connected
	 * to ensure we saturate as many busses as possible */
	root_hubs = gmw_root_hub_enumerate (priv->devices);
	for (guint i = 0; i < priv->devices->len; i++) {
		for (guint j = 0; j < root_hubs->len; j++) {
			g_autofree gchar *key = NULL;
			GmwRootHub *rh = g_ptr_array_index (root_hubs, j);
			if (rh->devices->len <= i)
				continue;
			device = g_ptr_array_index (rh->devices, i);
			key = g_strdup_printf ("%04u", idx++);
			gmw_device_set_order_process (device, key);
			g_debug ("set sort key %s for [%02x:] %s", key,
				 rh->idx, gmw_device_get_block_path (device));
		}
	}
}

typedef struct {
	GmwPrivate	*priv;
	GmwDevice 	*device;
	GtkEntry	*entry;
	GtkDialog	*dialog;
} GmwRenumberHelper;

static void gmw_refresh_ui (GmwPrivate *priv);

static void
gmw_main_show_quirks (GmwPrivate *priv)
{
	g_print ("\t/*     hub      hub-port  parent-hub     child-device"
		 "   chps  dprt  chn  labl */\n");
	for (guint i = 0; i < priv->devices->len; i++) {
		g_autofree gchar *tmp = NULL;
		GmwDevice *device = g_ptr_array_index (priv->devices, i);
		tmp = gmw_device_get_quirk_string (device);
		g_print ("\t%s\n", tmp);
	}
}

static void
gmw_main_rename_response_cb (GtkDialog *dialog,
			     GtkResponseType response_id,
			     GmwRenumberHelper *helper)
{
	if (response_id == GTK_RESPONSE_OK) {
		const gchar *tmp;
		tmp = gtk_entry_get_text (helper->entry);
		if (tmp != NULL && tmp[0] != '\0') {
			g_debug ("renaming %s to %s",
				 gmw_device_get_hub_id (helper->device), tmp);
			gmw_device_set_hub_label (helper->device, tmp);
			gmw_device_list_sort (helper->priv);
			gmw_main_show_quirks (helper->priv);
		}
	}
	gmw_refresh_ui (helper->priv);
	g_object_unref (helper->device);
	g_free (helper);
	gtk_widget_destroy (GTK_WIDGET (dialog));
}

static void
gmw_main_rename_activate_cb (GtkEntry *entry, GmwRenumberHelper *helper)
{
	gtk_dialog_response (helper->dialog, GTK_RESPONSE_OK);
}

static gboolean
gmw_main_rename_labels_cb (GtkLabel *label, const gchar *uri, GmwPrivate *priv)
{
	GmwDevice *device;
	GmwRenumberHelper *helper;
	GtkWidget *w;
	const gchar *tmp;
	guint idx;

	/* convert the URI into a device */
	idx = atoi (uri);
	device = g_ptr_array_index (priv->devices, idx);
	if (device == NULL)
		return FALSE;

	/* create warning dialog */
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_main"));
	w = gtk_message_dialog_new (GTK_WINDOW (w),
				    GTK_DIALOG_DESTROY_WITH_PARENT |
				    GTK_DIALOG_MODAL |
				    GTK_DIALOG_USE_HEADER_BAR,
				    GTK_MESSAGE_INFO,
				    GTK_BUTTONS_OK_CANCEL,
				    /* TRANSLATORS: window title renaming labels */
				    "%s", _("New hub label"));

	/* helper object */
	helper = g_new0 (GmwRenumberHelper, 1);
	helper->priv = priv;
	helper->device = g_object_ref (device);
	helper->dialog = GTK_DIALOG (w);
	g_signal_connect (helper->dialog, "response",
			  G_CALLBACK (gmw_main_rename_response_cb), helper);
	helper->entry = GTK_ENTRY (gtk_entry_new ());
	g_signal_connect (helper->entry, "activate",
			  G_CALLBACK (gmw_main_rename_activate_cb), helper);

	/* add existing hub label */
	gtk_widget_set_visible (GTK_WIDGET (helper->entry), TRUE);
	w = gtk_message_dialog_get_message_area (GTK_MESSAGE_DIALOG (helper->dialog));
	gtk_box_pack_end (GTK_BOX (w), GTK_WIDGET (helper->entry), FALSE, FALSE, 0);
	tmp = gmw_device_get_hub_label (device);
	if (tmp != NULL)
		gtk_entry_set_text (helper->entry, tmp);

	gtk_window_present (GTK_WINDOW (helper->dialog));
	return TRUE;
}

static void
gmw_refresh_ui (GmwPrivate *priv)
{
	GtkWidget *grid;
	GtkWidget *w;
	const guint max_devices_per_column = 10;

	/* remove old children */
	grid = GTK_WIDGET (gtk_builder_get_object (priv->builder, "grid_status"));
	gtk_container_foreach (GTK_CONTAINER (grid),
			       (GtkCallback) gtk_widget_destroy, priv);

	/* add new children */
	for (guint i = 0; i < priv->devices->len; i++) {
		g_autofree gchar *label_markup = NULL;
		g_autofree gchar *label = NULL;
		g_autofree gchar *title = NULL;
		guint row = i % max_devices_per_column;
		guint col = (i / max_devices_per_column) * 4;
		GmwDevice *device = g_ptr_array_index (priv->devices, i);

		/* add label */
		w = gtk_label_new (NULL);
		g_signal_connect (w, "activate-link",
				  G_CALLBACK (gmw_main_rename_labels_cb), priv);
		if (col > 0)
			gtk_widget_set_margin_start (w, 30);
		if (gmw_device_get_hub_label (device) != NULL) {
			if (priv->rename_labels) {
				label = g_strdup_printf ("%s [%s<a href=\"%u\">?</a>]",
							 gmw_device_get_hub_id (device),
							 gmw_device_get_hub_label (device), i);
			} else {
				label = g_strdup_printf ("%s [%s]",
							 gmw_device_get_hub_id (device),
							 gmw_device_get_hub_label (device));
			}
		} else {
			if (priv->rename_labels) {
				label = g_strdup_printf ("%s [<a href=\"%u\">?</a>]",
							 gmw_device_get_hub_id (device), i);
			} else {
				label = g_strdup (gmw_device_get_hub_id (device));
			}
		}
		label_markup = g_strdup_printf ("<tt><small>%s</small></tt>", label);
		gtk_label_set_markup (GTK_LABEL (w), label_markup);
		g_object_set (w, "xalign", 1.f, NULL);
		gtk_grid_attach (GTK_GRID (grid), w, col + 0, row, 1, 1);

		/* add icon */
		w = gtk_image_new ();
		gtk_image_set_from_icon_name (GTK_IMAGE (w),
					      gmw_device_get_icon (device),
					      GTK_ICON_SIZE_BUTTON);
		gtk_grid_attach (GTK_GRID (grid), w, col + 1, row, 1, 1);

		/* add optional progressbar */
		if (gmw_device_get_complete (device) > 0.f) {
			w = gtk_progress_bar_new ();
			gtk_widget_set_valign (w, GTK_ALIGN_CENTER);
			gtk_grid_attach (GTK_GRID (grid), w, col + 2, row, 1, 1);
			if (gmw_device_get_complete (device) <= 100.f) {
				gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (w),
							       gmw_device_get_complete (device));
			} else {
				gtk_progress_bar_pulse (GTK_PROGRESS_BAR (w));
			}
		}

		/* add optional status text */
		title = gmw_device_get_description (device);
		if (title != NULL) {
			w = gtk_label_new (title);
			g_object_set (w, "xalign", 0.f, NULL);
			gtk_label_set_width_chars (GTK_LABEL (w), 20);
			gtk_grid_attach (GTK_GRID (grid), w, col + 3, row, 1, 1);
		}
	}

	gtk_widget_show_all (grid);

	/* allow changing settings */
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_settings"));
	gtk_widget_set_sensitive (w, g_thread_pool_get_num_threads (priv->thread_pool) == 0);
	gtk_widget_set_visible (w, priv->devices->len > 0);

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

static void
gmw_refresh_in_idle (GmwPrivate *priv)
{
	g_mutex_lock (&priv->idle_id_mutex);
	if (priv->idle_id != 0)
		g_source_remove (priv->idle_id);
	priv->idle_id = g_timeout_add (50, gmw_refresh_in_idle_cb, priv);
	g_mutex_unlock (&priv->idle_id_mutex);
}

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
	g_autoptr(GError) error_local = NULL;
	g_autofree guchar *buffer_unaligned = NULL;
	g_autoptr(GOutputStream) device_stream = NULL;
	g_autoptr(GUnixFDList) fd_list = NULL;
	g_autoptr(GTimer) timer = NULL;
	g_autoptr(GVariant) fd_index = NULL;

	/* get fd from udisks */
	if (!udisks_block_call_open_for_restore_sync (gmw_device_get_udisks_block (device),
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
		bytes_total = gmw_device_get_size (device);
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
			gmw_device_set_speed_write (device, (gdouble) bytes_throughput / elapsed);
			g_timer_reset (timer);
			bytes_throughput = 0;
		}
		gmw_device_set_complete_write (device, (gdouble) bytes_completed /
						       (gdouble) bytes_total);
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
	g_autoptr(GError) error_local = NULL;
	g_autofree guchar *buffer_unaligned_src = NULL;
	g_autofree guchar *buffer_unaligned_dest = NULL;
	g_autoptr(GInputStream) device_stream = NULL;
	g_autoptr(GUnixFDList) fd_list = NULL;
	g_autoptr(GTimer) timer = NULL;
	g_autoptr(GVariant) fd_index = NULL;

	/* rewind */
	if (!g_seekable_seek (G_SEEKABLE (image_stream), 0,
			      G_SEEK_SET, cancellable, error))
		goto out;

	/* get fd from udisks */
	if (!udisks_block_call_open_for_backup_sync (gmw_device_get_udisks_block (device),
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
				     "Failed to verify at %" G_GUINT64_FORMAT "Mb",
				     bytes_completed / (1024 * 1024));
			goto out;
		}

		bytes_completed += bytes_read;
		bytes_throughput += bytes_read;

		/* update UI */
		elapsed = g_timer_elapsed (timer, NULL);
		if (elapsed > 1.f) {
			gmw_device_set_speed_read (device, (gdouble) bytes_throughput / elapsed);
			g_timer_reset (timer);
			bytes_throughput = 0;
		}

		/* update UI */
		gmw_device_set_complete_read (device, (gdouble) bytes_completed /
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

static void
gmw_refresh_titlebar (GmwPrivate *priv)
{
	GtkWidget *w;
	gdouble speed_read = 0.f;
	gdouble speed_write = 0.f;
	g_autoptr(GString) title = NULL;

	/* find the throughput totals */
	for (guint i = 0; i < priv->devices->len; i++) {
		GmwDevice *device = g_ptr_array_index (priv->devices, i);
		speed_read += gmw_device_get_speed_read (device);
		speed_write += gmw_device_get_speed_write (device);
	}

	/* set the title */
	title = g_string_new (_("MultiWriter"));
	if (speed_write > 0.f) {
		g_string_append_printf (title, " → %.0f MB/s",
					speed_write / (1000 * 1000));
	}
	if (speed_read > 0.f) {
		g_autofree gchar *tmp = NULL;
		tmp = g_strdup_printf ("%.0f MB/s → ",
				       speed_read / (1000 * 1000));
		g_string_prepend (title, tmp);
	}
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "header"));
	gtk_header_bar_set_title (GTK_HEADER_BAR (w), title->str);
}

static gboolean
gmw_refresh_titlebar_idle_cb (gpointer user_data)
{
	GmwPrivate *priv = (GmwPrivate *) user_data;
	gmw_refresh_titlebar (priv);
	return G_SOURCE_REMOVE;
}

static gboolean
gmw_block_device_probe (const gchar *block_dev, GError **error)
{
	gboolean ret;
	gint exit_status = 0;
	g_autofree gchar *standard_output = NULL;
	const gchar *argv[] = { "/usr/bin/pkexec",
				LIBEXECDIR"/gnome-multi-writer-probe",
				block_dev,
				NULL };
	const gchar *envp[] = { NULL };

	ret = g_spawn_sync (NULL, (gchar **) argv, (gchar **) envp,
			    G_SPAWN_STDERR_TO_DEV_NULL,
			    NULL, NULL,
			    &standard_output, NULL, &exit_status, error);
	if (!ret)
		return FALSE;
	g_strdelimit (standard_output, "\n", '\0');
	if (exit_status != 0) {
		g_set_error_literal (error, 1, 0, standard_output);
		return FALSE;
	}
	return TRUE;
}

static void
gmw_copy_thread_cb (gpointer data, gpointer user_data)
{
	GmwDevice *device = (GmwDevice *) data;
	GmwPrivate *priv = (GmwPrivate *) user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GInputStream) image_stream = NULL;

	/* set the factor for the write process */
	gmw_device_set_write_alloc (device, 0.75f);
	if (!g_settings_get_boolean (priv->settings, "enable-verify"))
		gmw_device_set_write_alloc (device, 1.f);

	/* probe the devices */
	if (g_settings_get_boolean (priv->settings, "enable-probe")) {
		const gchar *block_dev;
		gmw_device_set_state (device, GMW_DEVICE_STATE_VERIFY);
		block_dev = gmw_device_get_block_path (device);
		if (!gmw_block_device_probe (block_dev, &error)) {
			gmw_device_set_error (device, error);
			goto out;
		}
	}

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
	if (image_stream != NULL)
		g_input_stream_close (G_INPUT_STREAM (image_stream), NULL, NULL);
	gmw_copy_done (priv);
}

static void
gmw_update_title (GmwPrivate *priv)
{
	GtkWidget *w;
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "header"));
	if (priv->image_file == NULL) {
		gtk_header_bar_set_subtitle (GTK_HEADER_BAR (w), NULL);
	} else {
		g_autofree gchar *basename = NULL;
		basename = g_file_get_basename (priv->image_file);
		gtk_header_bar_set_subtitle (GTK_HEADER_BAR (w), basename);
	}
}

static void
gmw_set_image_file_changed_cb (GFileMonitor *monitor,
			       GFile *file,
			       GFile *other_file,
			       GFileMonitorEvent event_type,
			       GmwPrivate *priv)
{
	g_debug ("ISO file changed");
}

static void
gmw_set_image_filename (GmwPrivate *priv, const gchar *filename)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GFileInfo) info = NULL;

	if (priv->image_file != NULL)
		g_object_unref (priv->image_file);
	if (priv->image_monitor != NULL)
		g_object_unref (priv->image_monitor);
	priv->image_file = g_file_new_for_path (filename);
	priv->image_monitor = g_file_monitor (priv->image_file,
					      G_FILE_MONITOR_NONE,
					      NULL, &error);
	if (priv->image_monitor == NULL) {
		/* TRANSLATORS: we couldn't open the ISO file the user chose */
		gmw_error_dialog (priv, _("Failed to open"), error->message);
		return;
	}
	g_signal_connect (priv->image_monitor, "changed",
			  G_CALLBACK (gmw_set_image_file_changed_cb), priv);

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

static void
gmw_import_filename (GmwPrivate *priv)
{
	GtkFileFilter *filter = NULL;
	GtkWidget *d;
	GtkWidget *w;
	g_autoptr(GError) error = NULL;
	g_autoptr(GFile) file = NULL;

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
		g_autofree gchar *filename = NULL;
		filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (d));
		gmw_set_image_filename (priv, filename);
		gmw_update_title (priv);
		g_settings_set_string (priv->settings, "filename", filename);
	}
	gtk_widget_destroy (d);
}

static gboolean
gmw_auth_dummy_restore (GmwPrivate *priv, GmwDevice *device, GError **error)
{
	gboolean ret = FALSE;
	gint fd = -1;
	g_autoptr(GUnixFDList) fd_list = NULL;
	g_autoptr(GVariant) fd_index = NULL;

	if (!udisks_block_call_open_for_restore_sync (gmw_device_get_udisks_block (device),
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

static gboolean
gmw_throughput_update_titlebar_cb (gpointer user_data)
{
	GmwPrivate *priv = (GmwPrivate *) user_data;
	gmw_refresh_titlebar (priv);
	return G_SOURCE_CONTINUE;
}

static void
gmw_udisks_unmount_cb (GObject *source_object,
		       GAsyncResult *res,
		       gpointer user_data)
{
	UDisksFilesystem *udisks_fs = UDISKS_FILESYSTEM (source_object);
	g_autoptr(GError) error = NULL;

	if (!udisks_filesystem_call_unmount_finish (udisks_fs, res, &error))
		g_warning ("Failed to unmount filesystem: %s", error->message);
}

static UDisksFilesystem *
gmw_udisks_get_filesystem_for_device (GmwPrivate *priv, GmwDevice *device)
{
	/* this is crude, but fast -- LiveUSB devices will only typically have
	 * one partition, but very occasionaly two or more */
	for (guint i = 1; i <= 4; i++) {
		UDisksFilesystem *udisks_fs = NULL;
		g_autoptr(GError) error = NULL;
		g_autofree gchar *object_path = NULL;
		_cleanup_object_unref_ UDisksBlock *udisks_block = NULL;
		_cleanup_object_unref_ UDisksObject *udisks_object = NULL;
		g_auto(GStrv) mtab = NULL;

		object_path = g_strdup_printf ("%s%u",
					       gmw_device_get_object_path (device),
					       i);
		udisks_object = udisks_client_get_object (priv->udisks_client,
							  object_path);
		if (udisks_object == NULL)
			continue;
		udisks_fs = udisks_object_get_filesystem (udisks_object);
		if (udisks_fs == NULL)
			continue;
		mtab = udisks_filesystem_dup_mount_points (udisks_fs);
		if (mtab == NULL || mtab[0] == NULL) {
			g_debug ("%s not mounted", object_path);
			continue;
		}
		g_debug ("found filesystem %s from %s", mtab[0], object_path);
		return udisks_fs;
	}
	return NULL;
}

static void
gmw_udisks_unmount_filesystems_async (GmwPrivate *priv, GmwDevice *device)
{
	_cleanup_object_unref_ UDisksFilesystem *udisks_fs = NULL;
	udisks_fs = gmw_udisks_get_filesystem_for_device (priv, device);
	if (udisks_fs == NULL)
		return;
	udisks_filesystem_call_unmount (udisks_fs,
					g_variant_new ("a{sv}", NULL),
					priv->cancellable,
					gmw_udisks_unmount_cb,
					device);
}

static gboolean
gmw_udisks_unmount_filesystems_sync (GmwPrivate *priv, GmwDevice *device, GError **error)
{
	_cleanup_object_unref_ UDisksFilesystem *udisks_fs = NULL;
	udisks_fs = gmw_udisks_get_filesystem_for_device (priv, device);
	if (udisks_fs == NULL)
		return TRUE;
	return udisks_filesystem_call_unmount_sync (udisks_fs,
						    g_variant_new ("a{sv}", NULL),
						    priv->cancellable,
						    error);
}

static void
gmw_start_copy (GmwPrivate *priv)
{
	GtkWindow *window;
	const gchar *action_id = "org.freedesktop.udisks2.open-device";
	g_autoptr(GError) error = NULL;
	g_autoptr(GPermission) permission = NULL;

	/* if nothing already set, request this now */
	if (priv->image_file == NULL)
		gmw_import_filename (priv);
	if (priv->image_file == NULL)
		return;

	g_cancellable_reset (priv->cancellable);

	/* unmount all filesystems */
	for (guint i = 0; i < priv->devices->len; i++) {
		GmwDevice *device = g_ptr_array_index (priv->devices, i);
		if (!gmw_udisks_unmount_filesystems_sync (priv, device, &error)) {
			gmw_error_dialog (priv, _("Failed to copy"), error->message);
			return;
		}
	}


	/* do a dummy call to get the PolicyKit auth */
	permission = polkit_permission_new_sync (action_id, NULL, NULL, &error);
	if (permission == NULL) {
		gmw_error_dialog (priv, "Failed to get permission", error->message);
		return;
	}
	if (!g_permission_get_allowed (permission)) {
		GmwDevice *device = g_ptr_array_index (priv->devices, 0);
		if (!gmw_auth_dummy_restore (priv, device, &error)) {
			g_dbus_error_strip_remote_error (error);
			gmw_error_dialog (priv,
					  /* TRANSLATORS: error dialog title:
					   * we probably didn't authenticate */
					  _("Failed to copy"),
					  error->message);
			return;
		}
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
	for (guint i = 0; i < priv->devices->len; i++) {
		g_autofree gchar *title = NULL;
		GmwDevice *device = g_ptr_array_index (priv->devices, i);
		gmw_device_set_state (device, GMW_DEVICE_STATE_WAITING);
		g_thread_pool_push (priv->thread_pool, device, &error);
	}
	gmw_refresh_ui (priv);
}

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

static void
gmw_start_clicked_cb (GtkWidget *widget, GmwPrivate *priv)
{
	GtkWidget *w;
	GtkWidget *b;
	GtkStyleContext *context;
	g_autoptr(GString) str = g_string_new ("");

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

static void
gmw_import_activated_cb (GSimpleAction *action,
			 GVariant *parameter,
			 gpointer user_data)
{
	GmwPrivate *priv = (GmwPrivate *) user_data;
	gmw_import_filename (priv);
}

static void
gmw_about_activated_cb (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	GmwPrivate *priv = (GmwPrivate *) user_data;
	GList *windows;
	GtkWindow *parent = NULL;
	const gchar *authors[] = { "Richard Hughes", NULL };
	const gchar *copyright = "Copyright \xc2\xa9 2014-2018 Richard Hughes";

	windows = gtk_application_get_windows (GTK_APPLICATION (priv->application));
	if (windows)
		parent = windows->data;

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
			       "logo-icon-name", "org.gnome.MultiWriter",
			       /* TRANSLATORS: you can put your name here :) */
			       "translator-credits", _("translator-credits"),
			       "version", VERSION,
			       NULL);
}

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
	if (gtk_widget_get_parent (priv->switch_probe) != NULL)
		g_object_ref (priv->switch_probe);
	gtk_widget_unparent (priv->switch_probe);

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
	gtk_grid_attach (GTK_GRID (box),
			 /* TRANSLATORS: a switch label: we check the device
			  * is actually the size it says it is */
			 gtk_label_new (_("Probe")),
			 0, 2, 1, 1);
	gtk_grid_attach (GTK_GRID (box), priv->switch_probe, 1, 2, 1, 1);
	gtk_container_add (GTK_CONTAINER (pop), box);
	gtk_widget_show_all (pop);
}

static void
gmw_startup_cb (GApplication *application, GmwPrivate *priv)
{
	GtkWidget *main_window;
	GtkWidget *w;
	gint retval;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *filename = NULL;
	g_autoptr(GdkPixbuf) pixbuf = NULL;

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
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_import"));
	g_signal_connect (w, "clicked",
			  G_CALLBACK (gmw_import_clicked_cb), priv);

	/* setup USB image */
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "image_usb"));
	gtk_image_set_from_resource (GTK_IMAGE (w),
				     "/org/gnome/MultiWriter/usb-symbolic.svg");
	gtk_image_set_pixel_size (GTK_IMAGE (w), 48);

	/* get previously loaded image */
	filename = g_settings_get_string (priv->settings, "filename");
	if (filename != NULL && filename[0] != '\0')
		gmw_set_image_filename (priv, filename);

	/* show main UI */
	gmw_update_title (priv);
	gmw_refresh_ui (priv);
	gtk_widget_show (main_window);
}

static guint8
gmw_sysfs_get_busnum (const gchar *filename)
{
	g_autofree gchar *data = NULL;
	g_autofree gchar *path = NULL;
	path = g_build_filename (filename, "busnum", NULL);
	if (!g_file_get_contents (path, &data, NULL, NULL))
		return 0;
	return g_ascii_strtoull (data, NULL, 10);
}

static guint8
gmw_sysfs_get_devnum (const gchar *filename)
{
	g_autofree gchar *data = NULL;
	g_autofree gchar *path = NULL;
	path = g_build_filename (filename, "devnum", NULL);
	if (!g_file_get_contents (path, &data, NULL, NULL))
		return 0;
	return g_ascii_strtoull (data, NULL, 10);
}

static void
gmw_udisks_find_usb_device (GmwPrivate *priv, GmwDevice *device)
{
	guint8 busnum;
	guint8 devnum;
	g_autoptr(GError) error = NULL;
	g_autoptr(GUsbDevice) usb_device = NULL;

	/* failed to load GUsb */
	if (priv->usb_ctx == NULL)
		return;

	/* find the USB device using GUsb */
	if (gmw_device_get_sysfs_path (device) == NULL)
		return;
	busnum = gmw_sysfs_get_busnum (gmw_device_get_sysfs_path (device));
	devnum = gmw_sysfs_get_devnum (gmw_device_get_sysfs_path (device));
	if (busnum == 0x00 || devnum == 0x00) {
		g_warning ("Failed to get busnum for %s", gmw_device_get_sysfs_path (device));
		return;
	}
	usb_device = g_usb_context_find_by_bus_address (priv->usb_ctx,
							busnum,
							devnum,
							&error);
	if (usb_device == NULL) {
		g_warning ("%s", error->message);
		return;
	}
	gmw_device_set_usb_device (device, usb_device);
}

static gboolean
gmw_udisks_object_add (GmwPrivate *priv, GDBusObject *dbus_object)
{
	GmwDevice *device;
	const gchar *block_path;
	const gchar *object_path;
	guint64 device_size;
	g_autoptr(GDBusInterface) iface_block = NULL;
	g_autoptr(GDBusInterface) iface_fs = NULL;
	g_autoptr(GDBusInterface) iface_part = NULL;
	_cleanup_object_unref_ UDisksBlock *udisks_block = NULL;
	_cleanup_object_unref_ UDisksDrive *udisks_drive = NULL;
	_cleanup_object_unref_ UDisksObjectInfo *object_info = NULL;
	_cleanup_object_unref_ UDisksObject *udisks_object = NULL;

	/* is this the kind of device that interests us? */
	object_path = g_dbus_object_get_object_path (dbus_object);
	iface_block = g_dbus_object_get_interface (dbus_object, "org.freedesktop.UDisks2.Block");
	if (iface_block == NULL) {
		g_debug ("%s has no org.freedesktop.UDisks2.Block", object_path);
		return FALSE;
	}
	iface_part = g_dbus_object_get_interface (dbus_object, "org.freedesktop.UDisks2.Partition");
	if (iface_part != NULL) {
		g_debug ("%s has no org.freedesktop.UDisks2.Partition", object_path);
		return FALSE;
	}

	/* get the block device */
	udisks_object = udisks_client_get_object (priv->udisks_client, object_path);
	udisks_block = udisks_object_get_block (udisks_object);
	if (udisks_block == NULL) {
		g_debug ("%s has no block device", object_path);
		return FALSE;
	}

	/* ignore system devices */
	block_path = udisks_block_get_device (udisks_block);
	if (udisks_block_get_hint_system (udisks_block)) {
		g_debug ("%s is a system device", block_path);
		return FALSE;
	}

	/* ignore SD cards */
	if (g_str_has_prefix (block_path, "/dev/mmcblk")) {
		g_debug ("%s is not a USB device", block_path);
		return FALSE;
	}

	/* ignore small or large devices */
	device_size = udisks_block_get_size (udisks_block) / (1000 * 1000);
	if (device_size < 1000) {
		g_debug ("%s is too small [%u]",
			 block_path, (guint) device_size);
		return FALSE;
	}
	if (device_size > 1024 * 1024) {
		g_debug ("%s is too large [%u]",
			 block_path, (guint) device_size);
		return FALSE;
	}

	/* get the drive */
	udisks_drive = udisks_client_get_drive_for_block (priv->udisks_client, udisks_block);
	if (udisks_drive == NULL) {
		g_debug ("%s has no drive", block_path);
		return FALSE;
	}

	/* add this */
	object_info = udisks_client_get_object_info (priv->udisks_client, udisks_object);
	device = gmw_device_new ();
	gmw_device_set_name (device, udisks_object_info_get_name (object_info));
	gmw_device_set_block_path (device, block_path);
	gmw_device_set_object_path (device, object_path);
	gmw_device_set_udisks_block (device, udisks_block);

	/* set a connection ID */
	gmw_device_set_udisks_drive (device, udisks_drive);
	gmw_udisks_find_usb_device (priv, device);

	/* lazily unmount filesystems on the block device */
	gmw_udisks_unmount_filesystems_async (priv, device);

	g_mutex_lock (&priv->devices_mutex);
	g_ptr_array_add (priv->devices, device);
	g_mutex_unlock (&priv->devices_mutex);
	g_debug ("Added %s [%" G_GUINT64_FORMAT "]", block_path, device_size);
	return TRUE;
}

static void
gmw_update_max_threads (GmwPrivate *priv)
{
	guint nr_root = 1; /* assume there is at least one root hub */
	guint threads_per_root;
	g_autoptr(GPtrArray) root_hubs = NULL;

	/* get setting */
	threads_per_root = g_settings_get_uint (priv->settings, "max-threads");

	/* count root hubs in use */
	root_hubs = gmw_root_hub_enumerate (priv->devices);
	if (root_hubs->len > 0)
		nr_root = root_hubs->len;
	g_debug ("%u root %s in use", nr_root, nr_root > 1 ? "hubs" : "hub");
	g_thread_pool_set_max_threads (priv->thread_pool,
				       threads_per_root * nr_root,
				       NULL);
}

static void
gmw_udisks_object_added_cb (GDBusObjectManager *object_manager,
			    GDBusObject *dbus_object,
			    GmwPrivate *priv)
{
	if (gmw_udisks_object_add (priv, dbus_object)) {
		gmw_device_list_sort (priv);
		gmw_update_max_threads (priv);
		gmw_refresh_ui (priv);
	}
}

static void
gmw_udisks_object_removed_cb (GDBusObjectManager *object_manager,
			      GDBusObject *dbus_object,
			      GmwPrivate *priv)
{
	const gchar *tmp = g_dbus_object_get_object_path (dbus_object);
	for (guint i = 0; i < priv->devices->len; i++) {
		GmwDevice *device = g_ptr_array_index (priv->devices, i);
		if (g_strcmp0 (gmw_device_get_object_path (device), tmp) == 0) {
			g_ptr_array_remove (priv->devices, device);
			gmw_device_list_sort (priv);
			gmw_update_max_threads (priv);
			gmw_refresh_ui (priv);
			break;
		}
	}
}

static void
gmw_udisks_client_connect_cb (GObject *source_object,
			      GAsyncResult *res,
			      gpointer user_data)
{
	GmwPrivate *priv = (GmwPrivate *) user_data;
	GDBusObjectManager *object_manager;
	GList *objects;
	g_autoptr(GError) error = NULL;

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
	for (GList *l = objects; l != NULL; l = l->next)
		gmw_udisks_object_add (priv, G_DBUS_OBJECT (l->data));
	gmw_device_list_sort (priv);
	gmw_update_max_threads (priv);
	gmw_refresh_ui (priv);
	g_list_free_full (objects, (GDestroyNotify) g_object_unref);
}

static void
gmw_settings_changed_cb (GSettings *settings, const gchar *key, GmwPrivate *priv)
{
	if (g_strcmp0 (key, "blank-drive") == 0) {
		g_settings_set_boolean (settings, "show-warning", TRUE);
		return;
	}
	if (g_strcmp0 (key, "max-threads") == 0) {
		gmw_update_max_threads (priv);
		return;
	}
}

static gint
gmw_thread_pool_sort_func (gconstpointer a, gconstpointer b, gpointer user_data)
{
	GmwDevice *deva = (GmwDevice *) a;
	GmwDevice *devb = (GmwDevice *) b;
	return g_strcmp0 (gmw_device_get_order_process (deva),
			  gmw_device_get_order_process (devb));
}

static void
gmw_priv_free (GmwPrivate *priv)
{
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
	if (priv->image_monitor != NULL)
		g_object_unref (priv->image_monitor);
	if (priv->cancellable != NULL)
		g_object_unref (priv->cancellable);
	if (priv->application != NULL)
		g_object_unref (priv->application);
	g_thread_pool_free (priv->thread_pool, TRUE, TRUE);
	g_mutex_clear (&priv->thread_pool_mutex);
	g_mutex_clear (&priv->devices_mutex);
	g_mutex_clear (&priv->idle_id_mutex);
	g_ptr_array_unref (priv->devices);
	g_free (priv);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GmwPrivate, gmw_priv_free)

int
main (int argc, char **argv)
{
	gboolean verbose = FALSE;
	g_autoptr(GError) error = NULL;
	g_autoptr(GmwPrivate) priv = g_new0 (GmwPrivate, 1);
	g_autoptr(GOptionContext) context = NULL;
	const GOptionEntry options[] = {
		{ "rename-labels", '\0', 0, G_OPTION_ARG_NONE, &priv->rename_labels,
			/* TRANSLATORS: command line option */
			_("Allow renaming the labels on hubs"), NULL },
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
		/* TRANSLATORS: the user has sausages for fingers */
		g_print ("%s: %s\n", _("Failed to parse command line options"),
			 error->message);
		return EXIT_FAILURE;
	}

	g_mutex_init (&priv->thread_pool_mutex);
	g_mutex_init (&priv->devices_mutex);
	g_mutex_init (&priv->idle_id_mutex);
	priv->cancellable = g_cancellable_new ();
	priv->settings = g_settings_new ("org.gnome.MultiWriter");
	g_signal_connect (priv->settings, "changed",
			  G_CALLBACK (gmw_settings_changed_cb), priv);
	priv->thread_pool = g_thread_pool_new (gmw_copy_thread_cb, priv,
					       1, FALSE, &error);
	if (priv->thread_pool == NULL) {
		g_print ("Failed to create thread pool: %s\n", error->message);
		return EXIT_FAILURE;
	}
	g_thread_pool_set_sort_function (priv->thread_pool,
					 gmw_thread_pool_sort_func,
					 priv);
	priv->devices = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	priv->usb_ctx = g_usb_context_new (&error);
	if (priv->usb_ctx == NULL) {
		g_print ("Failed to load libusb: %s\n", error->message);
		return EXIT_FAILURE;
	}

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
	priv->switch_probe = gtk_switch_new ();
	g_settings_bind (priv->settings, "enable-verify",
			 priv->switch_verify, "active",
			 G_SETTINGS_BIND_DEFAULT);
	g_settings_bind (priv->settings, "blank-drive",
			 priv->switch_blank, "active",
			 G_SETTINGS_BIND_DEFAULT);
	g_settings_bind (priv->settings, "enable-probe",
			 priv->switch_probe, "active",
			 G_SETTINGS_BIND_DEFAULT);

	/* wait */
	return g_application_run (G_APPLICATION (priv->application), argc, argv);
}
