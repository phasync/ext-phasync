/* Pseudo-fibers (proof of concept): Fiber objects whose behaviour a PHP driver supplies.
 *
 * phasync\pseudo_fiber() returns a real \Fiber object that is never started as a Zend fiber: its
 * object handlers mark it, and Fiber's methods called on it go to the driver registered with
 * phasync\pseudo_fiber_driver(). Fiber::getCurrent() and Fiber::suspend() also ask the driver when
 * no real fiber is running. This lets a runtime whose tasks are not Zend fibers (Swoole coroutines)
 * hand out \Fiber handles for them: phasync's Fiber-typed API keeps its types.
 *
 * The driver is an object with these methods:
 *   start(Fiber $f, array $args): mixed      resume(Fiber $f, mixed $value): mixed
 *   throw(Fiber $f, Throwable $e): mixed     getReturn(Fiber $f): mixed
 *   status(Fiber $f): string                 one of "init", "suspended", "running", "terminated"
 *   current(): ?Fiber                        suspend(mixed $value): mixed
 *
 * phasync\fiber_error($message) throws a FiberError, which PHP code cannot construct.
 *
 * PoC limits: one driver per process (NTS); a pseudo-fiber's __construct() is not blocked. */

static zend_object_handlers phasync_pf_handlers;
static bool phasync_pf_handlers_ready;
static zval phasync_pf_driver;   /* IS_UNDEF when none */

enum { PF_START, PF_RESUME, PF_THROW, PF_GETRETURN, PF_ISSTARTED, PF_ISSUSPENDED, PF_ISRUNNING,
	PF_ISTERMINATED, PF_SUSPEND, PF_GETCURRENT, PF_COUNT };
static const char *phasync_pf_names[PF_COUNT] = { "start", "resume", "throw", "getreturn", "isstarted",
	"issuspended", "isrunning", "isterminated", "suspend", "getcurrent" };
static zif_handler phasync_pf_orig[PF_COUNT];

static bool phasync_pf_is(zval *this_ptr)
{
	return this_ptr && Z_TYPE_P(this_ptr) == IS_OBJECT && Z_OBJ_HT_P(this_ptr) == &phasync_pf_handlers;
}

/* Call $driver->name(...params) into return_value. */
static void phasync_pf_call(const char *name, zval *return_value, uint32_t n, zval *params)
{
	zval fn;
	ZVAL_STRING(&fn, name);
	call_user_function(NULL, &phasync_pf_driver, &fn, return_value, n, params);
	zval_ptr_dtor(&fn);
}

static void phasync_pf_status(INTERNAL_FUNCTION_PARAMETERS, const char *want)
{
	zval st;
	ZEND_PARSE_PARAMETERS_NONE();
	phasync_pf_call("status", &st, 1, ZEND_THIS);
	if (EG(exception)) {
		zval_ptr_dtor(&st);
		RETURN_THROWS();
	}
	if (Z_TYPE(st) != IS_STRING) {
		zval_ptr_dtor(&st);
		zend_throw_error(NULL, "Pseudo-fiber driver: status() must return a string");
		RETURN_THROWS();
	}
	bool r = want == NULL ? !zend_string_equals_literal(Z_STR(st), "init") : zend_string_equals_cstr(Z_STR(st), want, strlen(want));
	zval_ptr_dtor(&st);
	RETURN_BOOL(r);
}

#define PF_PASS(i) do { if (!phasync_pf_is(ZEND_THIS)) { phasync_pf_orig[i](INTERNAL_FUNCTION_PARAM_PASSTHRU); return; } } while (0)

static ZEND_NAMED_FUNCTION(phasync_pf_start)
{
	PF_PASS(PF_START);
	zval *args = NULL, params[2];
	uint32_t argc = 0;
	ZEND_PARSE_PARAMETERS_START(0, -1)
		Z_PARAM_VARIADIC('*', args, argc)
	ZEND_PARSE_PARAMETERS_END();
	ZVAL_COPY_VALUE(&params[0], ZEND_THIS);
	array_init_size(&params[1], argc);
	for (uint32_t i = 0; i < argc; i++) {
		Z_TRY_ADDREF(args[i]);
		zend_hash_next_index_insert_new(Z_ARRVAL(params[1]), &args[i]);
	}
	phasync_pf_call("start", return_value, 2, params);
	zval_ptr_dtor(&params[1]);
}

static ZEND_NAMED_FUNCTION(phasync_pf_resume)
{
	PF_PASS(PF_RESUME);
	zval *value = NULL, params[2];
	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(value)
	ZEND_PARSE_PARAMETERS_END();
	ZVAL_COPY_VALUE(&params[0], ZEND_THIS);
	if (value) {
		ZVAL_COPY_VALUE(&params[1], value);
	} else {
		ZVAL_NULL(&params[1]);
	}
	phasync_pf_call("resume", return_value, 2, params);
}

static ZEND_NAMED_FUNCTION(phasync_pf_throw)
{
	PF_PASS(PF_THROW);
	zval *e, params[2];
	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_OBJECT_OF_CLASS(e, zend_ce_throwable)
	ZEND_PARSE_PARAMETERS_END();
	ZVAL_COPY_VALUE(&params[0], ZEND_THIS);
	ZVAL_COPY_VALUE(&params[1], e);
	phasync_pf_call("throw", return_value, 2, params);
}

static ZEND_NAMED_FUNCTION(phasync_pf_getreturn)
{
	PF_PASS(PF_GETRETURN);
	ZEND_PARSE_PARAMETERS_NONE();
	phasync_pf_call("getReturn", return_value, 1, ZEND_THIS);
}

static ZEND_NAMED_FUNCTION(phasync_pf_isstarted)    { PF_PASS(PF_ISSTARTED);    phasync_pf_status(INTERNAL_FUNCTION_PARAM_PASSTHRU, NULL); }
static ZEND_NAMED_FUNCTION(phasync_pf_issuspended)  { PF_PASS(PF_ISSUSPENDED);  phasync_pf_status(INTERNAL_FUNCTION_PARAM_PASSTHRU, "suspended"); }
static ZEND_NAMED_FUNCTION(phasync_pf_isrunning)    { PF_PASS(PF_ISRUNNING);    phasync_pf_status(INTERNAL_FUNCTION_PARAM_PASSTHRU, "running"); }
static ZEND_NAMED_FUNCTION(phasync_pf_isterminated) { PF_PASS(PF_ISTERMINATED); phasync_pf_status(INTERNAL_FUNCTION_PARAM_PASSTHRU, "terminated"); }

/* Static methods: a real fiber wins; outside one, the driver answers. */
static ZEND_NAMED_FUNCTION(phasync_pf_suspend)
{
	if (EG(active_fiber) || Z_ISUNDEF(phasync_pf_driver)) {
		phasync_pf_orig[PF_SUSPEND](INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	zval *value = NULL, param;
	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(value)
	ZEND_PARSE_PARAMETERS_END();
	if (value) {
		ZVAL_COPY_VALUE(&param, value);
	} else {
		ZVAL_NULL(&param);
	}
	phasync_pf_call("suspend", return_value, 1, &param);
}

static ZEND_NAMED_FUNCTION(phasync_pf_getcurrent)
{
	if (EG(active_fiber) || Z_ISUNDEF(phasync_pf_driver)) {
		phasync_pf_orig[PF_GETCURRENT](INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	ZEND_PARSE_PARAMETERS_NONE();
	phasync_pf_call("current", return_value, 0, NULL);
}

static const zif_handler phasync_pf_hooks[PF_COUNT] = { phasync_pf_start, phasync_pf_resume, phasync_pf_throw,
	phasync_pf_getreturn, phasync_pf_isstarted, phasync_pf_issuspended, phasync_pf_isrunning,
	phasync_pf_isterminated, phasync_pf_suspend, phasync_pf_getcurrent };

/* function pseudo_fiber_driver(?object $driver): void */
static ZEND_FUNCTION(phasync_pseudo_fiber_driver)
{
	zval *driver;
	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_OBJECT_OR_NULL(driver)
	ZEND_PARSE_PARAMETERS_END();
	zval_ptr_dtor(&phasync_pf_driver);
	if (driver) {
		ZVAL_COPY(&phasync_pf_driver, driver);
	} else {
		ZVAL_UNDEF(&phasync_pf_driver);
	}
}

/* function pseudo_fiber(): \Fiber */
static ZEND_FUNCTION(phasync_pseudo_fiber)
{
	ZEND_PARSE_PARAMETERS_NONE();
	if (Z_ISUNDEF(phasync_pf_driver)) {
		zend_throw_error(NULL, "phasync\\pseudo_fiber(): no driver; call phasync\\pseudo_fiber_driver() first");
		RETURN_THROWS();
	}
	object_init_ex(return_value, zend_ce_fiber);
	if (!phasync_pf_handlers_ready) {
		memcpy(&phasync_pf_handlers, Z_OBJ_HT_P(return_value), sizeof(zend_object_handlers));
		phasync_pf_handlers_ready = true;
	}
	Z_OBJ_P(return_value)->handlers = &phasync_pf_handlers;
}

/* function fiber_error(string $message): never -- FiberError can't be instantiated from PHP */
static ZEND_FUNCTION(phasync_fiber_error)
{
	zend_string *msg;
	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(msg)
	ZEND_PARSE_PARAMETERS_END();
	zend_throw_error(zend_hash_str_find_ptr(CG(class_table), ZEND_STRL("fibererror")), "%s", ZSTR_VAL(msg));
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_phasync_fiber_error, 0, 1, IS_NEVER, 0)
	ZEND_ARG_TYPE_INFO(0, message, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_phasync_pseudo_fiber_driver, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, driver, IS_OBJECT, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_phasync_pseudo_fiber, 0, 0, Fiber, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry phasync_pf_functions[] = {
	ZEND_RAW_FENTRY("phasync\\pseudo_fiber_driver", ZEND_FN(phasync_pseudo_fiber_driver), arginfo_phasync_pseudo_fiber_driver, 0, NULL, NULL)
	ZEND_RAW_FENTRY("phasync\\fiber_error", ZEND_FN(phasync_fiber_error), arginfo_phasync_fiber_error, 0, NULL, NULL)
	ZEND_RAW_FENTRY("phasync\\pseudo_fiber", ZEND_FN(phasync_pseudo_fiber), arginfo_phasync_pseudo_fiber, 0, NULL, NULL)
	ZEND_FE_END
};

static void phasync_pf_minit(int module_number)
{
	zend_register_functions(NULL, phasync_pf_functions, NULL, MODULE_PERSISTENT);
	ZVAL_UNDEF(&phasync_pf_driver);
	for (int i = 0; i < PF_COUNT; i++) {
		zend_internal_function *f = zend_hash_str_find_ptr(&zend_ce_fiber->function_table, phasync_pf_names[i], strlen(phasync_pf_names[i]));
		phasync_pf_orig[i] = f->handler;
		f->handler = phasync_pf_hooks[i];
	}
}

static void phasync_pf_rshutdown(void)
{
	zval_ptr_dtor(&phasync_pf_driver);
	ZVAL_UNDEF(&phasync_pf_driver);
}
