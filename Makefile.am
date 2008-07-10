## Process this file with automake to produce Makefile.in

SUBDIRS = data docs librazor src po

GIT=./missing --run git --git-dir=$(srcdir)/.git

$(srcdir)/ChangeLog:
	if test $@ -ot $(srcdir)/.git/$$($(GIT) rev-parse --symbolic-full-name HEAD); then \
		git-log --stat > $@;		\
	fi

.PHONY: $(srcdir)/ChangeLog

EXTRA_DIST = 			\
	TODO	 		\
	ChangeLog 		\
	intltool-extract.in 	\
	intltool-merge.in 	\
	intltool-update.in

DISTCLEANFILES = \
	intltool-extract	\
	intltool-merge		\
	intltool-update

clean-local :
	rm -f *~

