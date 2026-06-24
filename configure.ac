#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.59)
AC_INIT([recpt1], [1.2.1.1], yaz@honeyplanet.jp)
AC_CONFIG_SRCDIR([recpt1.c])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC
AC_PROG_CXX

# Checks for b25 support.
AC_ARG_ENABLE(b25,
  [AS_HELP_STRING([--enable-b25],[enable b25 support])],
  [AC_CHECK_LIB([arib25], [create_arib_std_b25], , [AC_MSG_WARN(libarb25 is not available.)])]
)

# Checks for ACAS 4K passthrough support.
# ACAS/MMT-TLV parsing and card handling are derived from dantto4k:
# https://github.com/nekohkr/dantto4k.git
# Local source revision: 141fd49cf674c9c9e3f3c770a20b3291ff292969
AC_ARG_ENABLE(acas,
  [AS_HELP_STRING([--disable-acas],[disable ACAS 4K MMT/TLV passthrough descrambling])],
  [],
  [enable_acas=yes]
)

ACAS_OBJS=
ACAS_CPPFLAGS=
ACAS_LIBS=
AS_IF([test "x$enable_acas" = "xyes"], [
  AC_LANG_PUSH([C++])
  AC_CHECK_HEADERS([winscard.h PCSC/winscard.h])
  AS_IF([test "x$ac_cv_header_winscard_h" != "xyes" && test "x$ac_cv_header_PCSC_winscard_h" != "xyes"], [
    AC_MSG_ERROR([PC/SC winscard header is required for ACAS support. Use --disable-acas to build without it.])
  ])
  acas_save_LIBS="$LIBS"
  AC_SEARCH_LIBS([SCardEstablishContext], [pcsclite], [
    AC_DEFINE([HAVE_ACAS], [1], [Define if ACAS passthrough support is enabled])
    ACAS_OBJS="acas_passthrough.o dantto_aes.o"
    dnl dantto_aes.o is built directly from dantto4k/src/aes.cpp.
    ACAS_CPPFLAGS="-DHAVE_ACAS -I../dantto4k/src"
    ACAS_LIBS="$ac_cv_search_SCardEstablishContext"
    AS_IF([test "x$ACAS_LIBS" = "xnone required"], [ACAS_LIBS=""])
  ], [
    AC_MSG_ERROR([PC/SC library is required for ACAS support. Use --disable-acas to build without it.])
  ])
  LIBS="$acas_save_LIBS"
  AC_LANG_POP([C++])
])
AC_SUBST([ACAS_OBJS])
AC_SUBST([ACAS_CPPFLAGS])
AC_SUBST([ACAS_LIBS])

# Checks for libraries.
AC_CHECK_LIB([m], [log10])
AC_CHECK_LIB([pthread], [pthread_kill])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
