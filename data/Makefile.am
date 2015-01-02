SUBDIRS = 						\
	appdata

@INTLTOOL_DESKTOP_RULE@
desktopdir = $(datadir)/applications
desktop_in_files =					\
	org.gnome.MultiWriter.desktop.in
desktop_DATA = $(desktop_in_files:.desktop.in=.desktop)

@GSETTINGS_RULES@
gsettings_SCHEMAS = org.gnome.MultiWriter.gschema.xml

UI_FILES =						\
	gmw-main.ui					\
	gmw-menus.ui

EXTRA_DIST =						\
	$(gsettings_SCHEMAS)				\
	$(desktop_in_files)				\
	$(pkgdata_DATA)					\
	$(UI_FILES)

clean-local :
	rm -f *~

DISTCLEANFILES =					\
	org.gnome.MultiWriter.desktop			\
	org.gnome.MultiWriter.gschema.xml.valid

-include $(top_srcdir)/git.mk
