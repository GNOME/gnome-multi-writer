SUBDIRS = 						\
	appdata						\
	icons

@INTLTOOL_DESKTOP_RULE@
desktopdir = $(datadir)/applications
desktop_in_files =					\
	org.gnome.MultiWriter.desktop.in
desktop_DATA = $(desktop_in_files:.desktop.in=.desktop)

@GSETTINGS_RULES@
gsettings_SCHEMAS = org.gnome.MultiWriter.gschema.xml

@INTLTOOL_POLICY_RULE@
polkit_policydir = $(datadir)/polkit-1/actions
polkit_policy_DATA =					\
	org.gnome.MultiWriter.policy

UI_FILES =						\
	gmw-main.ui					\
	gmw-menus.ui

SVG_FILES =						\
	usb.svg

EXTRA_DIST =						\
	org.gnome.MultiWriter.policy.in			\
	$(desktop_in_files)				\
	$(gsettings_SCHEMAS)				\
	$(pkgdata_DATA)					\
	$(SVG_FILES)					\
	$(UI_FILES)

clean-local :
	rm -f *~

DISTCLEANFILES =					\
	org.gnome.MultiWriter.desktop			\
	org.gnome.MultiWriter.policy			\
	org.gnome.MultiWriter.gschema.xml.valid

-include $(top_srcdir)/git.mk
