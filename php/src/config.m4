PHP_ARG_ENABLE([opdump],
  [whether to enable opdump support],
  [AS_HELP_STRING([--enable-opdump], [Enable opdump (Phase 0 bytecode dump/reload spike)])],
  [yes])

if test "$PHP_OPDUMP" != "no"; then
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
