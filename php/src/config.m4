PHP_ARG_ENABLE([opdump],
  [whether to enable opdump support],
  [AS_HELP_STRING([--enable-opdump], [Enable opdump (Phase 0 bytecode dump/reload spike)])],
  [yes])

if test "$PHP_OPDUMP" != "no"; then
  PHP_ADD_LIBRARY([crypto], 1, [OPDUMP_SHARED_LIBADD])
  PHP_NEW_EXTENSION(opdump, opdump.c, $ext_shared)
  PHP_SUBST([OPDUMP_SHARED_LIBADD])
fi
