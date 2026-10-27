PHP_ARG_ENABLE([phasync],
  [whether to enable phasync support],
  [AS_HELP_STRING([--enable-phasync], [Enable phasync (epoll/kqueue Poller + async stream hooks)])],
  [no])

if test "$PHP_PHASYNC" != "no"; then
  phasync_vernum=`$PHP_CONFIG --vernum 2>/dev/null`
  if test -n "$phasync_vernum" && test "$phasync_vernum" -lt 80300; then
    phasync_ver=`$PHP_CONFIG --version`
    AC_MSG_ERROR([ext-phasync needs PHP 8.3 or newer, this is PHP $phasync_ver. phasync itself runs on PHP 8.2 without the extension.])
  fi
  AC_DEFINE(HAVE_PHASYNC, 1, [ phasync extension enabled ])
  dnl timer_create() and shm_open() are in librt before glibc 2.34
  PHP_CHECK_LIBRARY(rt, timer_create, [PHP_ADD_LIBRARY(rt, 1, PHASYNC_SHARED_LIBADD)])
  PHP_CHECK_LIBRARY(rt, shm_open, [PHP_ADD_LIBRARY(rt, 1, PHASYNC_SHARED_LIBADD)])
  dnl macOS: dns_search()/res_* are in libresolv
  PHP_CHECK_LIBRARY(resolv, dns_search, [PHP_ADD_LIBRARY(resolv, 1, PHASYNC_SHARED_LIBADD)])
  PHP_SUBST(PHASYNC_SHARED_LIBADD)
  dnl bundled fiber-interop/ext-scope (scope/, a git subtree): its own translation
  dnl unit, not #included into phasync.c's unity build, so its many generic
  dnl static names (scope_ce, scope_create, ...) never need to agree with ours.
  dnl COMPILE_DL_SCOPE is deliberately left undefined: its ZEND_GET_MODULE(scope)
  dnl must not compile in, since phasync.so is the only module and phasync.c
  dnl drives scope_module_entry itself (see phasync.c, PHP_MINIT(phasync)).
  PHP_NEW_EXTENSION(phasync, phasync.c scope/scope.c, $ext_shared,, -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1)
fi
