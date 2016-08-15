AM_CPPFLAGS =						\
	$(CANBERRA_CFLAGS)				\
	$(GLIB_CFLAGS)					\
	$(GTK_CFLAGS)					\
	$(GUDEV_CFLAGS)					\
	$(GUSB_CFLAGS)					\
	$(SOUP_CFLAGS)					\
	$(UDISKS_CFLAGS)				\
	-DG_LOG_DOMAIN=\"GnomeMultiWriter\"		\
	-DG_USB_API_IS_SUBJECT_TO_CHANGE		\
	-DLOCALEDIR=\""$(localedir)"\"

bin_PROGRAMS =						\
	gnome-multi-writer-probe			\
	gnome-multi-writer

gnome_multi_writer_LDADD =				\
	$(CANBERRA_LIBS)				\
	$(GLIB_LIBS)					\
	$(GTK_LIBS)					\
	$(GUSB_LIBS)					\
	$(UDISKS_LIBS)

gnome_multi_writer_CFLAGS =				\
	$(WARN_CFLAGS)

gnome_multi_writer_SOURCES =				\
	gmw-cleanup.h					\
	gmw-device.c					\
	gmw-device.h					\
	gmw-main.c					\
	gmw-resources.c					\
	gmw-resources.h

gnome_multi_writer_probe_LDADD =			\
	$(GLIB_LIBS)					\
	$(GUDEV_LIBS)

gnome_multi_writer_probe_CFLAGS =			\
	$(WARN_CFLAGS)

gnome_multi_writer_probe_SOURCES =			\
	gmw-cleanup.h					\
	gmw-probe.c

gmw-resources.c: gnome-multi-writer.gresource.xml ../data/gmw-main.ui ../data/gmw-menus.ui
	$(AM_V_GEN)					\
	glib-compile-resources --target=$@		\
	--sourcedir=$(top_srcdir)/data			\
	--generate-source				\
	--c-name gmw					\
	$(srcdir)/gnome-multi-writer.gresource.xml
gmw-resources.h: gnome-multi-writer.gresource.xml
	$(AM_V_GEN)					\
	glib-compile-resources --target=$@		\
	--sourcedir=$(top_srcdir)/data			\
	--generate-header				\
	--c-name gmw					\
	$(srcdir)/gnome-multi-writer.gresource.xml

BUILT_SOURCES =						\
	gmw-resources.c					\
	gmw-resources.h

EXTRA_DIST =						\
	gnome-multi-writer.gresource.xml

-include $(top_srcdir)/git.mk
