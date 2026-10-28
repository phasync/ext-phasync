/* This is a generated file, edit the .stub.php file instead.
 * Stub hash: 89b724e3796fd87f8b7bc63230aa6468e2ac8b98 */

/* Hand-maintained: never regenerate it with gen_stub (PHP 8.3 has no frameless
 * infos in ZEND_RAW_FENTRY, PHASYNC_RAW_FENTRY adapts). */
#if PHP_VERSION_ID >= 80400
# define PHASYNC_RAW_FENTRY ZEND_RAW_FENTRY
#else
# define PHASYNC_RAW_FENTRY(zend_name, name, arg_info, flags, frameless_infos, doc) ZEND_RAW_FENTRY(zend_name, name, arg_info, flags)
#endif
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_phasync_ext_Suspension_setPreemptInterval, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, seconds, IS_DOUBLE, 1)
ZEND_END_ARG_INFO()

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

#define arginfo_phasync_ext_stat_cache_stats arginfo_phasync_ext_io_stats

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_phasync_ext_Suspension___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_phasync_ext_Suspension_poll, 0, 2, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, suspensions, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, seconds, IS_DOUBLE, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_phasync_ext_Suspension_manage, 0, 1, Fiber, 0)
	ZEND_ARG_OBJ_INFO(0, fiber, Fiber, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_phasync_ext_Suspension_readable, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, stream, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout, IS_DOUBLE, 1, "null")
ZEND_END_ARG_INFO()

#define arginfo_class_phasync_ext_Suspension_writable arginfo_class_phasync_ext_Suspension_readable

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_phasync_ext_Suspension_suspend, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_phasync_ext_Suspension_getScope, 0, 0, Scope, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_phasync_ext_Suspension_getFiber, 0, 0, Fiber, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_phasync_ext_Suspension_isReady, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_phasync_ext_Suspension_resume, 0, 0, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_phasync_ext_Suspension_throw, 0, 1, IS_MIXED, 0)
	ZEND_ARG_OBJ_INFO(0, exception, Throwable, 0)
ZEND_END_ARG_INFO()

ZEND_FUNCTION(phasync_ext_sapi);
ZEND_FUNCTION(phasync_ext_run_next);
ZEND_FUNCTION(phasync_ext_dup2);
ZEND_FUNCTION(phasync_ext_include_global);
ZEND_FUNCTION(phasync_ext_io_stats);
ZEND_FUNCTION(phasync_ext_memo_stats);
ZEND_FUNCTION(phasync_ext_stat_cache_stats);
ZEND_METHOD(phasync_ext_Suspension, __construct);
ZEND_METHOD(phasync_ext_Suspension, poll);
ZEND_METHOD(phasync_ext_Suspension, manage);
ZEND_METHOD(phasync_ext_Suspension, readable);
ZEND_METHOD(phasync_ext_Suspension, writable);
ZEND_METHOD(phasync_ext_Suspension, suspend);
ZEND_METHOD(phasync_ext_Suspension, getScope);
ZEND_METHOD(phasync_ext_Suspension, getFiber);
ZEND_METHOD(phasync_ext_Suspension, isReady);
ZEND_METHOD(phasync_ext_Suspension, resume);
ZEND_METHOD(phasync_ext_Suspension, throw);
ZEND_METHOD(phasync_ext_Suspension, setPreemptInterval);

static const zend_function_entry ext_functions[] = {
	PHASYNC_RAW_FENTRY(ZEND_NS_NAME("phasync\\ext", "sapi"), zif_phasync_ext_sapi, arginfo_phasync_ext_sapi, 0, NULL, NULL)
	PHASYNC_RAW_FENTRY(ZEND_NS_NAME("phasync\\ext", "run_next"), zif_phasync_ext_run_next, arginfo_phasync_ext_run_next, 0, NULL, NULL)
	PHASYNC_RAW_FENTRY(ZEND_NS_NAME("phasync\\ext", "dup2"), zif_phasync_ext_dup2, arginfo_phasync_ext_dup2, 0, NULL, NULL)
	PHASYNC_RAW_FENTRY(ZEND_NS_NAME("phasync\\ext", "include_global"), zif_phasync_ext_include_global, arginfo_phasync_ext_include_global, 0, NULL, NULL)
	PHASYNC_RAW_FENTRY(ZEND_NS_NAME("phasync\\ext", "io_stats"), zif_phasync_ext_io_stats, arginfo_phasync_ext_io_stats, 0, NULL, NULL)
	PHASYNC_RAW_FENTRY(ZEND_NS_NAME("phasync\\ext", "memo_stats"), zif_phasync_ext_memo_stats, arginfo_phasync_ext_memo_stats, 0, NULL, NULL)
	PHASYNC_RAW_FENTRY(ZEND_NS_NAME("phasync\\ext", "stat_cache_stats"), zif_phasync_ext_stat_cache_stats, arginfo_phasync_ext_stat_cache_stats, 0, NULL, NULL)
	ZEND_FE_END
};

static const zend_function_entry class_phasync_ext_Suspension_methods[] = {
	ZEND_ME(phasync_ext_Suspension, __construct, arginfo_class_phasync_ext_Suspension___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(phasync_ext_Suspension, poll, arginfo_class_phasync_ext_Suspension_poll, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(phasync_ext_Suspension, manage, arginfo_class_phasync_ext_Suspension_manage, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(phasync_ext_Suspension, readable, arginfo_class_phasync_ext_Suspension_readable, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(phasync_ext_Suspension, writable, arginfo_class_phasync_ext_Suspension_writable, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(phasync_ext_Suspension, suspend, arginfo_class_phasync_ext_Suspension_suspend, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(phasync_ext_Suspension, getScope, arginfo_class_phasync_ext_Suspension_getScope, ZEND_ACC_PUBLIC)
	ZEND_ME(phasync_ext_Suspension, getFiber, arginfo_class_phasync_ext_Suspension_getFiber, ZEND_ACC_PUBLIC)
	ZEND_ME(phasync_ext_Suspension, isReady, arginfo_class_phasync_ext_Suspension_isReady, ZEND_ACC_PUBLIC)
	ZEND_ME(phasync_ext_Suspension, resume, arginfo_class_phasync_ext_Suspension_resume, ZEND_ACC_PUBLIC)
	ZEND_ME(phasync_ext_Suspension, throw, arginfo_class_phasync_ext_Suspension_throw, ZEND_ACC_PUBLIC)
	ZEND_ME(phasync_ext_Suspension, setPreemptInterval, arginfo_class_phasync_ext_Suspension_setPreemptInterval, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_FE_END
};

static zend_class_entry *phasync_register_class(zend_class_entry *ce, uint32_t flags)
{
#if PHP_VERSION_ID >= 80400
	return zend_register_internal_class_with_flags(ce, NULL, flags);
#else
	zend_class_entry *class_entry = zend_register_internal_class_ex(ce, NULL);
	class_entry->ce_flags |= flags;
	return class_entry;
#endif
}

static zend_class_entry *register_class_phasync_ext_Suspension(zend_class_entry *class_entry_Exception)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "phasync\\ext", "Suspension", class_phasync_ext_Suspension_methods);
#if PHP_VERSION_ID >= 80400
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_Exception, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_NOT_SERIALIZABLE);
#else
	class_entry = zend_register_internal_class_ex(&ce, class_entry_Exception);
	class_entry->ce_flags |= ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_NOT_SERIALIZABLE;
#endif

	return class_entry;
}

static zend_class_entry *register_class_phasync_Uninterruptible(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "phasync", "Uninterruptible", NULL);
	class_entry = phasync_register_class(&ce, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES);

	zend_string *attribute_name_Attribute_class_phasync_Uninterruptible_0 = zend_string_init_interned("Attribute", sizeof("Attribute") - 1, 1);
	zend_attribute *attribute_Attribute_class_phasync_Uninterruptible_0 = zend_add_class_attribute(class_entry, attribute_name_Attribute_class_phasync_Uninterruptible_0, 1);
	zend_string_release(attribute_name_Attribute_class_phasync_Uninterruptible_0);
	ZVAL_LONG(&attribute_Attribute_class_phasync_Uninterruptible_0->args[0].value, 6);

	return class_entry;
}
