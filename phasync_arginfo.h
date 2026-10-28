/* This is a generated file, edit the .stub.php file instead.
 * Stub hash: 919e9cd34b20cab9229ae40cab21b0f1ccdc47f8 */

/* Hand-maintained: never regenerate it with gen_stub (PHP 8.3 has no frameless
 * infos in ZEND_RAW_FENTRY, PHASYNC_RAW_FENTRY adapts). */
#if PHP_VERSION_ID >= 80400
# define PHASYNC_RAW_FENTRY ZEND_RAW_FENTRY
#else
# define PHASYNC_RAW_FENTRY(zend_name, name, arg_info, flags, frameless_infos, doc) ZEND_RAW_FENTRY(zend_name, name, arg_info, flags)
#endif

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_phasync_ext_sapi, 0, 2, IS_MIXED, 0)
	ZEND_ARG_OBJ_INFO(0, code, Closure, 0)
	ZEND_ARG_TYPE_INFO(0, sapi, IS_OBJECT, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_phasync_ext_run_next, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, script, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_phasync_ext_dup2, 0, 2, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, stream, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, fd, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_phasync_ext_include_global, 0, 1, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, file, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_phasync_ext_io_stats, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

#define arginfo_phasync_ext_memo_stats arginfo_phasync_ext_io_stats

#define arginfo_phasync_ext_gc_grace_stats arginfo_phasync_ext_io_stats

#define arginfo_phasync_ext_stat_cache_stats arginfo_phasync_ext_io_stats

ZEND_FUNCTION(phasync_ext_sapi);
ZEND_FUNCTION(phasync_ext_run_next);
ZEND_FUNCTION(phasync_ext_dup2);
ZEND_FUNCTION(phasync_ext_include_global);
ZEND_FUNCTION(phasync_ext_io_stats);
ZEND_FUNCTION(phasync_ext_memo_stats);
ZEND_FUNCTION(phasync_ext_gc_grace_stats);
ZEND_FUNCTION(phasync_ext_stat_cache_stats);

static const zend_function_entry ext_functions[] = {
	PHASYNC_RAW_FENTRY(ZEND_NS_NAME("phasync\\ext", "sapi"), zif_phasync_ext_sapi, arginfo_phasync_ext_sapi, 0, NULL, NULL)
	PHASYNC_RAW_FENTRY(ZEND_NS_NAME("phasync\\ext", "run_next"), zif_phasync_ext_run_next, arginfo_phasync_ext_run_next, 0, NULL, NULL)
	PHASYNC_RAW_FENTRY(ZEND_NS_NAME("phasync\\ext", "dup2"), zif_phasync_ext_dup2, arginfo_phasync_ext_dup2, 0, NULL, NULL)
	PHASYNC_RAW_FENTRY(ZEND_NS_NAME("phasync\\ext", "include_global"), zif_phasync_ext_include_global, arginfo_phasync_ext_include_global, 0, NULL, NULL)
	PHASYNC_RAW_FENTRY(ZEND_NS_NAME("phasync\\ext", "io_stats"), zif_phasync_ext_io_stats, arginfo_phasync_ext_io_stats, 0, NULL, NULL)
	PHASYNC_RAW_FENTRY(ZEND_NS_NAME("phasync\\ext", "memo_stats"), zif_phasync_ext_memo_stats, arginfo_phasync_ext_memo_stats, 0, NULL, NULL)
	PHASYNC_RAW_FENTRY(ZEND_NS_NAME("phasync\\ext", "gc_grace_stats"), zif_phasync_ext_gc_grace_stats, arginfo_phasync_ext_gc_grace_stats, 0, NULL, NULL)
	PHASYNC_RAW_FENTRY(ZEND_NS_NAME("phasync\\ext", "stat_cache_stats"), zif_phasync_ext_stat_cache_stats, arginfo_phasync_ext_stat_cache_stats, 0, NULL, NULL)
	ZEND_FE_END
};
