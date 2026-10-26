#ifndef PHASYNC_PHP_H
#define PHASYNC_PHP_H

/* The PHP headers and version shims phasync.c and the files it includes use. */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "php_main.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_streams.h"
#include "php_network.h"
#include "SAPI.h"
#include "zend_exceptions.h"
#include "zend_closures.h"
#include "zend_interfaces.h"
#include "ext/spl/spl_exceptions.h"
#include "ext/date/php_date.h"
#include "zend_observer.h"
#include <float.h>
#include "zend_attributes.h"
#include "zend_inheritance.h"
#include "zend_ini_scanner.h"
#include "zend_language_parser.h"
#if PHP_VERSION_ID >= 80400
#include "zend_frameless_function.h" /* issue #29: zend_flf_handlers/zend_flf_functions, PHP 8.4+ only */
#endif

#include <poll.h>
#include <errno.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/un.h>
#ifdef HAVE_ARPA_NAMESER_H
# ifdef __APPLE__
#  define BIND_8_COMPAT 1   /* HEADER, C_IN, GETSHORT: as PHP's dns.c there */
# endif
# include <arpa/nameser.h>
#endif
#ifdef HAVE_RESOLV_H
# include <resolv.h>
#endif
#ifdef HAVE_DNS_H
# include <dns.h>
#endif
#include "ext/standard/php_dns.h"
#include "ext/standard/php_filestat.h"
#include "ext/standard/basic_functions.h"
#include "ext/standard/file.h"
#include "rfc1867.h"
#include "php_variables.h"
#include "php_open_temporary_file.h"
#include "zend_observer.h"
#include "zend_fibers.h"
#include "zend_generators.h"
#include "zend_extensions.h"
#if __has_include("ext/session/php_session.h")
# include "ext/session/php_session.h"
# define PHASYNC_HAVE_SESSION 1
#endif
#if __has_include("ext/mysqli/php_mysqli_structs.h")
# include "ext/mysqli/php_mysqli_structs.h"
# define PHASYNC_HAVE_MYSQLI 1
#endif
#if __has_include("ext/pdo/php_pdo_driver.h")
# include "ext/pdo/php_pdo_driver.h"
# define PHASYNC_HAVE_PDO 1
#endif
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <locale.h>
#ifdef __APPLE__
# include <xlocale.h>   /* locale_t */
#endif
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <limits.h>
#include <dirent.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <sys/file.h>
#include <sys/uio.h>

/* Platform seam. Linux: epoll, eventfd, preadv2(RWF_NOWAIT), pidfd. Elsewhere
 * (macOS/BSD): kqueue behind the same event bits, no RWF_NOWAIT (local files
 * are read inline, as stock does). */
#ifdef __linux__
# include <sys/epoll.h>
# include <sys/eventfd.h>
# include <sys/syscall.h>
# include <sys/sysmacros.h>
# include <sys/vfs.h>
# ifndef SYS_preadv2
#  error "phasync needs preadv2(2) (Linux 4.6)"
# endif
# ifndef RWF_NOWAIT
#  define RWF_NOWAIT 0x00000008   /* Linux 4.14 */
# endif
# ifndef SYS_pidfd_open
#  define SYS_pidfd_open 434   /* Linux 5.3; same number on every architecture */
# endif
# define PHASYNC_HAVE_NOWAIT 1
# define PHASYNC_CLOCK_COARSE CLOCK_MONOTONIC_COARSE
# define PHASYNC_ST_MTIM(st) ((st).st_mtim)
# define PHASYNC_ST_CTIM(st) ((st).st_ctim)
# define PHASYNC_EV_IN      EPOLLIN
# define PHASYNC_EV_PRI     EPOLLPRI
# define PHASYNC_EV_OUT     EPOLLOUT
# define PHASYNC_EV_RDHUP   EPOLLRDHUP
# define PHASYNC_EV_HUP     EPOLLHUP
# define PHASYNC_EV_ERR     EPOLLERR
# define PHASYNC_EV_ONESHOT EPOLLONESHOT
typedef struct epoll_event phasync_event;
#else
# include <sys/event.h>
# include <sys/mount.h>
# define PHASYNC_HAVE_NOWAIT 0
# define RWF_NOWAIT 0
# ifdef __APPLE__
#  include <libproc.h>
#  include <mach/mach.h>
#  define PHASYNC_CLOCK_COARSE CLOCK_MONOTONIC_RAW_APPROX
int fdatasync(int);   /* in libc, undeclared by older SDKs (PHP declares it the same way) */
# else
#  define PHASYNC_CLOCK_COARSE CLOCK_MONOTONIC_FAST
# endif
# define PHASYNC_ST_MTIM(st) ((st).st_mtimespec)
# define PHASYNC_ST_CTIM(st) ((st).st_ctimespec)
# define PHASYNC_EV_IN      0x001u
# define PHASYNC_EV_PRI     0x002u
# define PHASYNC_EV_OUT     0x004u
# define PHASYNC_EV_ERR     0x008u
# define PHASYNC_EV_HUP     0x010u
# define PHASYNC_EV_RDHUP   0x2000u
# define PHASYNC_EV_ONESHOT (1u << 30)
/* What phasync_backend_wait() fills in from kevents, shaped as epoll's. */
typedef struct {
	uint32_t events;
	union { uint64_t u64; int fd; } data;
} phasync_event;
#endif

/* ext-phasync needs PHP 8.3+ (phasync itself runs on 8.2 without it). */
#if PHP_VERSION_ID < 80300
# error "ext-phasync needs PHP 8.3 or newer; phasync runs on PHP 8.2 without the extension"
#endif

/* PHP version seam: what the code takes from newer PHP, for PHP 8.3 and 8.4. */
#if PHP_VERSION_ID < 80400
# define zend_map_ptr_static_size 0              /* no static map_ptr area before 8.4 */
# define ZEND_VM_OP_LOOP_END      0x00000040     /* unused before 8.4: no opcode has it */
# define ZEND_ACC_PPP_SET_MASK    0              /* no asymmetric visibility */
# define ZEND_ACC_VIRTUAL         0              /* no property hooks */
# define Z_PARAM_FUNC_NO_TRAMPOLINE_FREE Z_PARAM_FUNC   /* the caller refetches a freed trampoline */
static zend_always_inline void zend_class_redeclaration_error_ex(int type, zend_string *name, zend_class_entry *old_ce)
{
	zend_error(type, "Cannot declare %s %s, because the name is already in use", zend_get_object_type(old_ce), ZSTR_VAL(name));
}
#endif
#if PHP_VERSION_ID < 80500
static zend_always_inline bool zend_ast_is_decl(const zend_ast *ast)
{
	return zend_ast_is_special((zend_ast *) ast) && ast->kind >= ZEND_AST_FUNC_DECL;
}
static zend_always_inline void zend_safe_assign_to_variable_noref(zval *variable_ptr, zval *value)
{
	if (Z_REFCOUNTED_P(variable_ptr)) {
		zend_refcounted *ref = Z_COUNTED_P(variable_ptr);
		ZVAL_COPY_VALUE(variable_ptr, value);
		GC_DTOR_NO_REF(ref);
	} else {
		ZVAL_COPY_VALUE(variable_ptr, value);
	}
}
#endif

#define PHP_PHASYNC_VERSION "2.0.3"

#endif /* PHASYNC_PHP_H */
