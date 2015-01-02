AUTOMAKE_OPTIONS = 1.7

ACLOCAL_AMFLAGS = -I m4

SUBDIRS = po data man src

DISTCHECK_CONFIGURE_FLAGS =				\
	--disable-bash-completion

EXTRA_DIST =						\
	COPYING						\
	MAINTAINERS					\
	AUTHORS						\
	INSTALL						\
	README						\
	NEWS						\
	autogen.sh					\
	config.h

MAINTAINERCLEANFILES =					\
	$(srcdir)/ABOUT-NLS				\
	$(srcdir)/INSTALL				\
	$(srcdir)/aclocal.m4				\
	$(srcdir)/autoscan.log				\
	$(srcdir)/compile				\
	$(srcdir)/config.guess				\
	$(srcdir)/config.rpath				\
	$(srcdir)/config.h.in				\
	$(srcdir)/config.sub				\
	$(srcdir)/configure.scan			\
	$(srcdir)/depcomp				\
	$(srcdir)/install-sh				\
	$(srcdir)/ltmain.sh				\
	$(srcdir)/missing				\
	$(srcdir)/mkinstalldirs				\
	$(srcdir)/omf.make				\
	$(srcdir)/ChangeLog				\
	$(srcdir)/gnome-multi-writer-*.tar.*		\
	`find "$(srcdir)" -type f -name Makefile.in -print`

distclean-local:
	if test $(srdcir) = .; then :; else \
		rm -f ChangeLog; \
	fi

ChangeLog:
	@echo Creating $@
	@if test -d "$(srcdir)/.git"; then \
	  (GIT_DIR=$(top_srcdir)/.git ./missing --run git log e197d80c8937c622f21f569457d1bbd05746d951.. --stat -M -C --name-status  --date=short --no-color) | fmt --split-only > $@.tmp \
	  && mv -f $@.tmp $@ \
	  || ($(RM) $@.tmp; \
	      echo Failed to generate ChangeLog, your ChangeLog may be outdated >&2; \
	      (test -f $@ || echo git-log is required to generate this file >> $@)); \
	else \
	  test -f $@ || \
	  (echo A git checkout and git-log is required to generate ChangeLog >&2 && \
	  echo A git checkout and git-log is required to generate this file >> $@); \
	fi

.PHONY: ChangeLog

-include $(top_srcdir)/git.mk
