PHP_ARG_ENABLE([opdump],
  [whether to enable opdump support],
  [AS_HELP_STRING([--enable-opdump], [Enable opdump (Phase 0 bytecode dump/reload spike)])],
  [yes])

if test "$PHP_OPDUMP" != "no"; then
  PHP_NEW_EXTENSION(opdump, opdump.c, $ext_shared)
fi
