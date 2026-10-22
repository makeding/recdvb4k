#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.59)
AC_INIT([recpt1], [1.2.1.1], yaz@honeyplanet.jp)
AC_CONFIG_SRCDIR([recpt1.c])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC
AC_PROG_CXX
PKG_PROG_PKG_CONFIG

# Checks for b25 support.
AC_ARG_ENABLE(b25,
  [AS_HELP_STRING([--enable-b25],[enable b25 support])],
  [AC_CHECK_LIB([arib25], [create_arib_std_b25], , [AC_MSG_WARN(libarb25 is not available.)])]
)

# Checks for B61/ACAS 4K passthrough support.
AC_ARG_ENABLE(b61,
  [AS_HELP_STRING([--enable-b61],[enable B61/ACAS 4K MMT/TLV descrambling support])],
  [],
  [enable_b61=no]
)
B61_CPPFLAGS=
B61_LIBS=
AS_IF([test "x$enable_b61" = "xyes"], [
  PKG_CHECK_MODULES([ARIBB61], [libaribb61], [], [
    AC_MSG_ERROR([libaribb61 is required for B61 support. Install libaribb61 or use --disable-b61.])
  ])
  AC_DEFINE([HAVE_B61], [1], [Define if B61/ACAS passthrough support is enabled])
  B61_CPPFLAGS="$ARIBB61_CFLAGS"
  B61_LIBS="$ARIBB61_LIBS"
])
AC_SUBST([B61_CPPFLAGS])
AC_SUBST([B61_LIBS])

# Checks for libraries.
AC_CHECK_LIB([m], [log10])
AC_CHECK_LIB([pthread], [pthread_kill])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
