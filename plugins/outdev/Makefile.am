if HAVE_OUTDEV

if HAVE_GTK2
if HAVE_GTK3
pkglib_LTLIBRARIES = outdev_gtk2.la outdev_gtk3.la
else
pkglib_LTLIBRARIES = outdev_gtk2.la
endif
else
if HAVE_GTK3
pkglib_LTLIBRARIES = outdev_gtk3.la
endif
endif

if HAVE_GTK2
outdev_gtk2_la_SOURCES = outdev.c
outdev_gtk2_la_LDFLAGS = -module -avoid-version
outdev_gtk2_la_LIBADD = $(LDADD) $(GTK2_DEPS_LIBS)
outdev_gtk2_la_CFLAGS = $(CFLAGS) -std=c99 $(GTK2_DEPS_CFLAGS) -I@top_srcdir@/include -fPIC
endif

if HAVE_GTK3
outdev_gtk3_la_SOURCES = outdev.c
outdev_gtk3_la_LDFLAGS = -module -avoid-version
outdev_gtk3_la_LIBADD = $(LDADD) $(GTK3_DEPS_LIBS)
outdev_gtk3_la_CFLAGS = $(CFLAGS) -std=c99 $(GTK3_DEPS_CFLAGS) -I@top_srcdir@/include -fPIC
endif

endif
