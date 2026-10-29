/* suspension/: blocking calls in fibers wait through an event loop (the
 * Suspension class, the hooks, the poller, the thread pool, preemption). Its own
 * translation unit (unit.c); this header is the rest of the extension's only way
 * in, and suspension/ includes nothing outside itself but PHP and system headers
 * (CI checks both). */
#ifndef PHASYNC_SUSPENSION_H
#define PHASYNC_SUSPENSION_H

#include "platform.h"

#define PHASYNC_SUSP_API __attribute__((visibility("hidden")))

/* Lifecycle, called from the module's own handlers at the points phasync.c shows. */
PHASYNC_SUSP_API zend_result phasync_susp_module_startup(int type, int module_number);
PHASYNC_SUSP_API void phasync_susp_module_shutdown(void);
PHASYNC_SUSP_API void phasync_susp_request_startup(int module_number);
PHASYNC_SUSP_API void phasync_susp_request_shutdown(void);
PHASYNC_SUSP_API void phasync_susp_post_deactivate(void);
PHASYNC_SUSP_API void phasync_susp_info(void);
ZEND_FUNCTION(phasync_ext_io_stats);   /* phasync\ext\io_stats(), in the module's function list */

/* For sapi(): a stream's fd; an internal function by name; the registered header
 * callback, which sapi() keeps per request; ext/session's globals (NULL without it). */
PHASYNC_SUSP_API php_socket_t phasync_stream_fd(php_stream *stream);
PHASYNC_SUSP_API zend_internal_function *phasync_find_ifunc(const char *name, size_t len);
PHASYNC_SUSP_API zend_function **phasync_header_cb_slot(void);
#ifdef PHASYNC_HAVE_SESSION
PHASYNC_SUSP_API extern php_ps_globals *phasync_ps;
#endif

/* For the stat cache: this process mutated the file system; the next lookup drains inotify. */
PHASYNC_SUSP_API extern bool phasync_sc_own;

#endif /* PHASYNC_SUSPENSION_H */
