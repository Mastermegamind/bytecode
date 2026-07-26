PHP_ARG_ENABLE([opdump],
  [whether to enable opdump support],
  [AS_HELP_STRING([--enable-opdump], [Enable opdump (Phase 0 bytecode dump/reload spike)])],
  [yes])

PHP_ARG_WITH([opdump-vendor-secret],
  [vendor secret hex compiled into the opdump loader],
  [AS_HELP_STRING([--with-opdump-vendor-secret=HEX], [Compile a 64-hex vendor secret fallback into the loader])],
  [no],
  [no])

if test "$PHP_OPDUMP" != "no"; then
  if test "$PHP_OPDUMP_VENDOR_SECRET" != "no" && test -n "$PHP_OPDUMP_VENDOR_SECRET"; then
    case "$PHP_OPDUMP_VENDOR_SECRET" in
      *[!0123456789abcdefABCDEF]*)
        AC_MSG_ERROR([--with-opdump-vendor-secret must be 64 hex characters])
        ;;
    esac
    if test ${#PHP_OPDUMP_VENDOR_SECRET} -ne 64; then
      AC_MSG_ERROR([--with-opdump-vendor-secret must be 64 hex characters])
    fi
    AC_DEFINE_UNQUOTED([OPDUMP_VENDOR_SECRET_HEX], ["$PHP_OPDUMP_VENDOR_SECRET"], [64-hex vendor secret compiled into the loader])
  fi

  AC_PATH_PROG([PKG_CONFIG], [pkg-config])
  if test -n "$PKG_CONFIG" && $PKG_CONFIG --exists openssl; then
    OPDUMP_OPENSSL_CFLAGS=`$PKG_CONFIG --cflags openssl`
    OPDUMP_OPENSSL_LIBS=`$PKG_CONFIG --libs openssl`
    PHP_EVAL_INCLINE([$OPDUMP_OPENSSL_CFLAGS])
    PHP_EVAL_LIBLINE([$OPDUMP_OPENSSL_LIBS], [OPDUMP_SHARED_LIBADD])
  else
    AC_PATH_PROG([BREW], [brew])
    if test -n "$BREW"; then
      OPDUMP_OPENSSL_PREFIX=`$BREW --prefix openssl@3 2>/dev/null`
      if test -n "$OPDUMP_OPENSSL_PREFIX" && test -d "$OPDUMP_OPENSSL_PREFIX"; then
        PHP_ADD_INCLUDE([$OPDUMP_OPENSSL_PREFIX/include])
        PHP_ADD_LIBRARY_WITH_PATH([crypto], [$OPDUMP_OPENSSL_PREFIX/lib], [OPDUMP_SHARED_LIBADD])
      else
        PHP_ADD_LIBRARY([crypto], 1, [OPDUMP_SHARED_LIBADD])
      fi
    else
      PHP_ADD_LIBRARY([crypto], 1, [OPDUMP_SHARED_LIBADD])
    fi
  fi
  PHP_NEW_EXTENSION(opdump, opdump.c, $ext_shared)
  PHP_SUBST([OPDUMP_SHARED_LIBADD])
fi
