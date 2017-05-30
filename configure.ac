AC_PREREQ([2.63])

AC_INIT([flatpak-xdg-utils],[0.0.1])

AX_IS_RELEASE([micro-version])

AC_CONFIG_SRCDIR([src/xdg-open.c])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([1.11 foreign subdir-objects tar-ustar no-dist-gzip dist-xz])

# Enable silent rules if available
AM_SILENT_RULES([yes])
AM_MAINTAINER_MODE([enable])

AX_CHECK_ENABLE_DEBUG([yes])
AC_USE_SYSTEM_EXTENSIONS
AX_COMPILER_FLAGS()

# i18n
GETTEXT_PACKAGE=AC_PACKAGE_TARNAME
AC_SUBST([GETTEXT_PACKAGE])
AC_DEFINE_UNQUOTED([GETTEXT_PACKAGE], ["$GETTEXT_PACKAGE"], [GETTEXT package name])

AM_GNU_GETTEXT_VERSION([0.18.3])
AM_GNU_GETTEXT([external])

# required programs
AC_PROG_CC
AC_PROG_INSTALL
PKG_PROG_PKG_CONFIG([0.24])

LT_PREREQ([2.2.6])
LT_INIT([disable-static])

PKG_CHECK_MODULES(FLATPAK_XDG_UTILS, [gio-2.0 >= 2.50 gio-unix-2.0])

AC_CONFIG_FILES([
  Makefile
  src/Makefile
  po/Makefile.in
])
AC_OUTPUT
