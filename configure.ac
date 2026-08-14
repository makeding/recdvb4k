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
# ACAS/MMT-TLV parsing and card handling are derived from dantto4k:
# https://github.com/nekohkr/dantto4k.git
# Local source revision: 141fd49cf674c9c9e3f3c770a20b3291ff292969
AC_ARG_ENABLE(b61,
  [AS_HELP_STRING([--enable-b61],[enable B61/ACAS 4K MMT/TLV descrambling support])],
  [],
  [enable_b61=no]
)
AC_ARG_WITH(asio-include,
  [AS_HELP_STRING([--with-asio-include=DIR],[path containing asio.hpp])],
  [ASIO_CPPFLAGS="-I$withval"],
  [ASIO_CPPFLAGS=""]
)

B61_OBJS=
B61_CPPFLAGS=
B61_LIBS=
B61_TEST_TARGET=
AS_IF([test "x$enable_b61" = "xyes"], [
  PKG_CHECK_MODULES([PCSC], [libpcsclite], [], [
    PCSC_CFLAGS=
    PCSC_LIBS=
  ])
  PCSC_PARENT_CFLAGS=
  for flag in $PCSC_CFLAGS; do
    case "$flag" in
      -I*/PCSC)
        PCSC_PARENT_CFLAGS="$PCSC_PARENT_CFLAGS -I${flag#-I}/.."
        ;;
    esac
  done
  AC_LANG_PUSH([C++])
  acas_save_CPPFLAGS="$CPPFLAGS"
  CPPFLAGS="$CPPFLAGS $PCSC_CFLAGS $PCSC_PARENT_CFLAGS $ASIO_CPPFLAGS"
  AC_CHECK_HEADERS([winscard.h PCSC/winscard.h asio.hpp])
  AS_IF([test "x$ac_cv_header_winscard_h" != "xyes" && test "x$ac_cv_header_PCSC_winscard_h" != "xyes"], [
    AC_MSG_ERROR([PC/SC winscard header is required for B61 support. Use --disable-b61 to build without it.])
  ])
  AS_IF([test "x$ac_cv_header_asio_hpp" != "xyes"], [
    AC_MSG_ERROR([standalone Asio is required for B61 proxy support. Use --with-asio-include=DIR.])
  ])
  acas_save_LIBS="$LIBS"
  LIBS="$LIBS $PCSC_LIBS"
  AC_SEARCH_LIBS([SCardEstablishContext], [pcsclite], [
    AC_DEFINE([HAVE_B61], [1], [Define if B61/ACAS passthrough support is enabled])
    B61_OBJS="acas_passthrough.o acas_state.o acas_card.o acas_sections.o acas_response.o acas_aes.o acas_cas_proxy_client.o"
    B61_TEST_TARGET="acas_test"
    B61_CPPFLAGS="-DHAVE_B61 -DASIO_STANDALONE -I. $PCSC_CFLAGS $PCSC_PARENT_CFLAGS $ASIO_CPPFLAGS"
    B61_LIBS="$PCSC_LIBS"
    AS_IF([test "x$ac_cv_search_SCardEstablishContext" != "xnone required"], [
      B61_LIBS="$B61_LIBS $ac_cv_search_SCardEstablishContext"
    ])
  ], [
    AC_MSG_ERROR([PC/SC library is required for B61 support. Use --disable-b61 to build without it.])
  ])
  LIBS="$acas_save_LIBS"
  CPPFLAGS="$acas_save_CPPFLAGS"
  AC_LANG_POP([C++])
])
AC_SUBST([B61_OBJS])
AC_SUBST([B61_CPPFLAGS])
AC_SUBST([B61_LIBS])
AC_SUBST([B61_TEST_TARGET])

# Checks for libraries.
AC_CHECK_LIB([m], [log10])
AC_CHECK_LIB([pthread], [pthread_kill])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
