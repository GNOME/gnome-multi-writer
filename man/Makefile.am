EXTRA_DIST =						\
	gnome-multi-writer.sgml

if HAVE_DOCBOOK2MAN
man_MANS =						\
	gnome-multi-writer.1
endif

if HAVE_DOCBOOK2MAN
gnome-multi-writer.1: gnome-multi-writer.sgml
	docbook2man $? > /dev/null
endif

clean-local :
	rm -f *~
	rm -f *.1
	rm -f manpage.*

CLEANFILES =						\
	manpage.links					\
	manpage.log					\
	manpage.refs					\
	$(man_MANS)

-include $(top_srcdir)/git.mk
