/* EXPERIMENT, built only with -DPHASYNC_EXP_MEMO_PROFILE, active only with env PHASYNC_MEMO_PROFILE=<prefix>:
 * which internal functions are memoization candidates across requests. Every internal call: its self time
 * (wall and thread CPU, minus nested observed calls and time switched out to other fibers), whether its
 * (function, args) key was seen before with the same result (and the same stat of a path argument),
 * objects/resources/references, output, errors, exceptions and global-state changes. Accumulated per
 * process, written to <prefix>.<pid>.tsv every 50 requests and at MSHUTDOWN. */
#include <sys/stat.h>
#include "main/php_output.h"
#include "SAPI.h"

typedef struct mp_stat {
	zend_string *name;
	uint64_t calls, wall, cpu, hash, rep, xrep, varies, statchg, paths, objarg, objres, byref, big, out, err, exc, glob, distinct, argbytes;
	struct mp_sizes *sz;   /* json_decode(), unserialize(), parse_url(), parse_ini_*(): input sizes, miss pre-filters */
	struct mp_stat *alt;   /* parse_url() with $component counts apart */
} mp_stat;
/* Per call: the string's length; a miss (key never seen) whose length no earlier call had, or whose
 * fingerprint (length, first and last 64 bytes) no earlier call had, a pre-filter would reject */
typedef struct mp_sizes { uint32_t *len; uint64_t n, cap, bytes, hits, misses, miss_newlen, miss_newfp; HashTable lens, fps; } mp_sizes;
typedef struct {
	zend_execute_data *ex;
	mp_stat *st;
	uint64_t key, sig, snap, enter, start, cpu, child, out;
	uint32_t err;
	bool ok, obj, big, ref;
} mp_frame;
typedef struct { mp_frame *f; uint32_t n, cap; uint64_t left; } mp_stack;
typedef struct { uint64_t key, res, sig; uint32_t req; } mp_seen;

static bool mp_on;
static pid_t mp_pid;
static char mp_path[1024];
static HashTable mp_stats, mp_byfn, mp_fibers;
static mp_stack mp_main, *mp_cur = &mp_main;
static mp_seen *mp_tab;
static uint64_t mp_tabcap, mp_tabn, mp_out, mp_rcpu, mp_rwall, mp_req_cpu, mp_req_wall;
static uint64_t mp_cal_wall, mp_cal_cpu;   /* the measuring's own cost inside an empty call's window */
static uint32_t mp_req, mp_err;
static int mp_budget;
static size_t (*mp_ub_write_orig)(const char *str, size_t len);

static uint64_t mp_clock(clockid_t c) { struct timespec t; clock_gettime(c, &t); return (uint64_t) t.tv_sec * 1000000000ULL + t.tv_nsec; }
#define mp_now() mp_clock(CLOCK_MONOTONIC)
static uint64_t mp_mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); return h; }

static size_t mp_ub_write(const char *str, size_t len) { mp_out += len; return mp_ub_write_orig(str, len); }
static void mp_error(int type, zend_string *file, uint32_t line, zend_string *message) { mp_err++; }

static uint64_t mp_hash(zval *zv, mp_frame *f, int depth)
{
	ZVAL_DEREF(zv);
	switch (Z_TYPE_P(zv)) {
		case IS_STRING: return mp_mix(zend_string_hash_val(Z_STR_P(zv)), Z_STRLEN_P(zv));
		case IS_LONG: return mp_mix(4, (uint64_t) Z_LVAL_P(zv));
		case IS_DOUBLE: { uint64_t u; double d = Z_DVAL_P(zv); memcpy(&u, &d, 8); return mp_mix(5, u); }
		case IS_ARRAY: {
			uint64_t h = 7;
			zend_string *k; zend_ulong i; zval *v;
			if (depth > 32 || (mp_budget -= zend_hash_num_elements(Z_ARRVAL_P(zv))) < 0) { f->ok = false; f->big = true; return 0; }
			ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(zv), i, k, v) {
				h = mp_mix(h, k ? zend_string_hash_val(k) : i);
				h = mp_mix(h, mp_hash(v, f, depth + 1));
			} ZEND_HASH_FOREACH_END();
			return h;
		}
		case IS_OBJECT: case IS_RESOURCE: f->ok = false; f->obj = true; return 0;
		default: return Z_TYPE_P(zv);
	}
}

/* Global state a call may change: ini, error handling, tables, headers, buffers */
static uint64_t mp_snapshot(void)
{
	uint64_t h = mp_mix(EG(error_reporting), EG(modified_ini_directives) ? zend_hash_num_elements(EG(modified_ini_directives)) : 0);
	h = mp_mix(h, zend_hash_num_elements(EG(function_table)));
	h = mp_mix(h, zend_hash_num_elements(EG(class_table)));
	h = mp_mix(h, zend_hash_num_elements(EG(zend_constants)));
	h = mp_mix(h, zend_hash_num_elements(&EG(included_files)));
	h = mp_mix(h, zend_hash_num_elements(&EG(symbol_table)));
	h = mp_mix(h, Z_TYPE(EG(user_error_handler)) | (Z_TYPE(EG(user_exception_handler)) << 8));
	h = mp_mix(h, zend_llist_count(&SG(sapi_headers).headers));
	return mp_mix(h, SG(sapi_headers).http_response_code);
}
static uint64_t mp_outstate(void)
{
	return mp_mix(mp_mix(mp_out, (uint64_t) (uintptr_t) OG(active)), OG(active) ? OG(active)->buffer.used : 0);
}

static mp_frame *mp_push(zend_execute_data *ex)
{
	mp_stack *s = mp_cur;
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 256;
		s->f = realloc(s->f, s->cap * sizeof(mp_frame));
	}
	mp_frame *f = &s->f[s->n++];
	f->ex = ex;
	f->child = 0;
	return f;
}
/* The frame of ex, popped (with any frames left above it); NULL if it is not on this fiber's stack */
static mp_frame *mp_pop(zend_execute_data *ex)
{
	mp_stack *s = mp_cur;
	for (uint32_t i = s->n; i-- > 0; ) {
		if (s->f[i].ex == ex) {
			s->n = i;
			return &s->f[i];
		}
	}
	return NULL;
}
static void mp_charge_parent(uint64_t elapsed)
{
	if (mp_cur->n) {
		mp_cur->f[mp_cur->n - 1].child += elapsed;
	}
}

static void mp_user_begin(zend_execute_data *ex) { mp_push(ex)->enter = mp_now(); }
static void mp_user_end(zend_execute_data *ex, zval *rv)
{
	uint64_t now = mp_now();
	mp_frame *f = mp_pop(ex);
	if (f) {
		mp_charge_parent(now - f->enter);
	}
}

static mp_stat *mp_stat_named(const char *buf, size_t n)
{
	mp_stat *st = zend_hash_str_find_ptr(&mp_stats, buf, n);
	if (!st) {
		st = calloc(1, sizeof *st);
		st->name = zend_string_init(buf, n, 1);
		zend_hash_add_new_ptr(&mp_stats, st->name, st);
		if (!strcmp(buf, "json_decode") || !strcmp(buf, "unserialize") || !strncmp(buf, "parse_url", 9) || !strncmp(buf, "parse_ini_", 10)) {
			st->sz = calloc(1, sizeof(mp_sizes));
			zend_hash_init(&st->sz->lens, 1024, NULL, NULL, 1);
			zend_hash_init(&st->sz->fps, 1024, NULL, NULL, 1);
		}
		if (!strcmp(buf, "parse_url")) {
			st->alt = mp_stat_named(ZEND_STRL("parse_url+component"));
		}
	}
	return st;
}
static mp_stat *mp_stat_of(zend_function *fn)
{
	mp_stat *st = zend_hash_index_find_ptr(&mp_byfn, (zend_ulong) (uintptr_t) fn);
	if (!st) {
		char buf[512];
		size_t n = snprintf(buf, sizeof buf, "%s%s%s", fn->common.scope ? ZSTR_VAL(fn->common.scope->name) : "",
			fn->common.scope ? "::" : "", ZSTR_VAL(fn->common.function_name));
		st = mp_stat_named(buf, n);
		zend_hash_index_add_new_ptr(&mp_byfn, (zend_ulong) (uintptr_t) fn, st);
	}
	return st;
}

static bool mp_is_path_arg(const char *name)
{
	return !strcmp(name, "filename") || !strcmp(name, "path") || !strcmp(name, "directory") || !strcmp(name, "file") || !strcmp(name, "dir");
}

static void mp_begin(zend_execute_data *ex)
{
	uint64_t enter = mp_now(), t;
	zend_function *fn = ex->func;
	mp_frame *f = mp_push(ex);
	uint32_t n = ZEND_CALL_NUM_ARGS(ex);
	uint64_t h;

	f->enter = enter;
	f->st = mp_stat_of(fn);
	if (f->st->alt && n >= 2) {
		f->st = f->st->alt;
	}
	f->ok = true; f->obj = f->big = f->ref = false;
	f->sig = 0;
	if (Z_TYPE(ex->This) == IS_OBJECT) {
		f->ok = false; f->obj = true;
	}
	mp_budget = 5000;
	h = mp_mix(11, (uint64_t) (uintptr_t) f->st);
	for (uint32_t i = 1; i <= n; i++) {
		if (ARG_SHOULD_BE_SENT_BY_REF(fn, i)) {
			f->ref = true;   /* an out (or in-out) argument: hashed with the result, after the call */
			continue;
		}
		h = mp_mix(h, mp_hash(ZEND_CALL_ARG(ex, i), f, 0));
		if (Z_TYPE_P(ZEND_CALL_ARG(ex, i)) == IS_STRING) {
			f->st->argbytes += Z_STRLEN_P(ZEND_CALL_ARG(ex, i));
		}
	}
	f->key = h;
	t = mp_now();
	f->st->hash += t - enter;
	for (uint32_t i = 1; i <= n && i <= fn->common.num_args; i++) {
		zval *a = ZEND_CALL_ARG(ex, i);
		struct stat sb;
		if (Z_TYPE_P(a) == IS_STRING && Z_STRLEN_P(a) && Z_STRLEN_P(a) < MAXPATHLEN && !memchr(Z_STRVAL_P(a), 0, Z_STRLEN_P(a))
			&& mp_is_path_arg(fn->internal_function.arg_info[i - 1].name)) {
			f->sig = stat(Z_STRVAL_P(a), &sb) ? mp_mix(1, errno)
				: mp_mix(mp_mix(mp_mix(mp_mix(sb.st_ino, sb.st_size), sb.st_mtim.tv_sec * 1000000000ULL + sb.st_mtim.tv_nsec),
					sb.st_ctim.tv_sec * 1000000000ULL + sb.st_ctim.tv_nsec), sb.st_mode ^ ((uint64_t) sb.st_dev << 32));
			f->st->paths++;
			break;
		}
	}
	f->snap = mp_snapshot();
	f->out = mp_outstate();
	f->err = mp_err;
	f->cpu = mp_clock(CLOCK_THREAD_CPUTIME_ID);
	f->start = mp_now();
}

static mp_seen *mp_lookup(uint64_t key)
{
	if (mp_tabn * 2 >= mp_tabcap) {
		mp_seen *old = mp_tab;
		uint64_t oc = mp_tabcap;
		mp_tabcap = oc ? oc * 2 : 1 << 20;
		mp_tab = calloc(mp_tabcap, sizeof(mp_seen));
		for (uint64_t i = 0; i < oc; i++) {
			if (old[i].key) {
				uint64_t j = old[i].key & (mp_tabcap - 1);
				while (mp_tab[j].key) j = (j + 1) & (mp_tabcap - 1);
				mp_tab[j] = old[i];
			}
		}
		free(old);
	}
	key |= 1;
	uint64_t j = key & (mp_tabcap - 1);
	while (mp_tab[j].key && mp_tab[j].key != key) j = (j + 1) & (mp_tabcap - 1);
	return &mp_tab[j];
}

static void mp_end(zend_execute_data *ex, zval *rv)
{
	uint64_t stop = mp_now(), cpu = mp_clock(CLOCK_THREAD_CPUTIME_ID);
	mp_frame *f = mp_pop(ex);
	mp_stat *st;

	if (!f) {
		return;
	}
	st = f->st;
	st->calls++;
	st->wall += stop - f->start > f->child + mp_cal_wall ? stop - f->start - f->child - mp_cal_wall : 0;
	st->cpu += cpu - f->cpu > f->child + mp_cal_cpu ? cpu - f->cpu - f->child - mp_cal_cpu : 0;
	st->out += mp_outstate() != f->out;
	st->err += mp_err != f->err;
	st->exc += EG(exception) != NULL;
	st->glob += mp_snapshot() != f->snap;
	if (st->sz && ZEND_CALL_NUM_ARGS(ex) && Z_TYPE_P(ZEND_CALL_ARG(ex, 1)) == IS_STRING) {
		mp_sizes *z = st->sz;
		zend_string *a = Z_STR_P(ZEND_CALL_ARG(ex, 1));
		size_t l = ZSTR_LEN(a), k = l < 64 ? l : 64;
		uint64_t fp = mp_mix(mp_mix(l, zend_inline_hash_func(ZSTR_VAL(a), k)), zend_inline_hash_func(ZSTR_VAL(a) + l - k, k));
		if (z->n == z->cap) {
			z->cap = z->cap ? z->cap * 2 : 4096;
			z->len = realloc(z->len, z->cap * sizeof(uint32_t));
		}
		z->len[z->n++] = (uint32_t) l;
		z->bytes += l;
		mp_seen *seen = mp_lookup(f->key ^ 0x5a5a5a5a5a5a5a5aULL);   /* its own key space: also calls whose result holds objects */
		if (seen->key) {
			z->hits++;
		} else {
			seen->key = (f->key ^ 0x5a5a5a5a5a5a5a5aULL) | 1;
			mp_tabn++;
			z->misses++;
			z->miss_newlen += !zend_hash_index_exists(&z->lens, l);
			z->miss_newfp += !zend_hash_index_exists(&z->fps, fp);
		}
		zend_hash_index_add_empty_element(&z->lens, l);
		zend_hash_index_add_empty_element(&z->fps, fp);
	}
	if (f->ok && rv && !EG(exception)) {
		uint64_t r;
		uint32_t n = ZEND_CALL_NUM_ARGS(ex);
		mp_budget = 5000;
		r = mp_hash(rv, f, 0);
		for (uint32_t i = 1; f->ref && i <= n; i++) {
			if (ARG_SHOULD_BE_SENT_BY_REF(ex->func, i)) {
				r = mp_mix(r, mp_hash(ZEND_CALL_ARG(ex, i), f, 0));
			}
		}
		if (f->ok) {
			mp_seen *s = mp_lookup(f->key);
			st->distinct += !s->key || s->req != mp_req;   /* first time this request */
			if (!s->key) {
				s->key = f->key | 1;
				mp_tabn++;
			} else if (s->sig != f->sig) {
				st->statchg++;
			} else if (s->res != r) {
				st->varies++;
			} else {
				st->rep++;
				st->xrep += s->req != mp_req;
			}
			s->res = r; s->sig = f->sig; s->req = mp_req;
		} else {
			st->objres += !f->big;
		}
	} else if (f->obj) {
		st->objarg++;
	}
	st->byref += f->ref;
	st->big += f->big;
	mp_charge_parent(mp_now() - f->enter);
}

static zend_observer_fcall_handlers mp_init(zend_execute_data *ex)
{
	zend_observer_fcall_handlers h = { mp_user_begin, mp_user_end };
	if (ex->func->type == ZEND_INTERNAL_FUNCTION) {
		h.begin = mp_begin;
		h.end = mp_end;
	}
	return h;
}

static mp_stack *mp_stack_of(zend_fiber_context *c, bool create)
{
	mp_stack *s = zend_hash_index_find_ptr(&mp_fibers, (zend_ulong) (uintptr_t) c);
	if (!s && create) {
		s = calloc(1, sizeof *s);
		zend_hash_index_add_new_ptr(&mp_fibers, (zend_ulong) (uintptr_t) c, s);
	}
	return s;
}
static void mp_fiber_switch(zend_fiber_context *from, zend_fiber_context *to)
{
	uint64_t now = mp_now();
	if (!zend_hash_index_exists(&mp_fibers, (zend_ulong) (uintptr_t) from)) {
		zend_hash_index_add_new_ptr(&mp_fibers, (zend_ulong) (uintptr_t) from, mp_cur);
	}
	mp_cur->left = now;
	mp_cur = mp_stack_of(to, true);
	if (mp_cur->n && mp_cur->left) {
		mp_cur->f[mp_cur->n - 1].child += now - mp_cur->left;   /* time in other fibers is not this call's */
	}
}
static void mp_fiber_destroy(zend_fiber_context *c)
{
	mp_stack *s = mp_stack_of(c, false);
	if (s && s != &mp_main && s != mp_cur) {
		free(s->f);
		free(s);
	}
	zend_hash_index_del(&mp_fibers, (zend_ulong) (uintptr_t) c);
}

static void mp_dump(void)
{
	char path[1100];
	FILE *o;
	mp_stat *st;

	snprintf(path, sizeof path, "%s.%d.tsv", mp_path, (int) getpid());
	if (!(o = fopen(path, "w"))) {
		return;
	}
	fprintf(o, "#requests\t%u\treq_cpu_ns\t%llu\treq_wall_ns\t%llu\tkeys\t%llu\tcal_wall\t%llu\tcal_cpu\t%llu\n", mp_req,
		(unsigned long long) mp_req_cpu, (unsigned long long) mp_req_wall, (unsigned long long) mp_tabn,
		(unsigned long long) mp_cal_wall, (unsigned long long) mp_cal_cpu);
	fprintf(o, "function\tcalls\twall_ns\tcpu_ns\thash_ns\trep\txrep\tvaries\tstatchg\tpaths\tobjarg\tobjres\tbyref\tbig\tout\terr\texc\tglob\tdistinct\targbytes\n");
	ZEND_HASH_FOREACH_PTR(&mp_stats, st) {
		if (st->calls) {
			fprintf(o, "%s\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n", ZSTR_VAL(st->name),
				(unsigned long long) st->calls, (unsigned long long) st->wall, (unsigned long long) st->cpu, (unsigned long long) st->hash,
				(unsigned long long) st->rep, (unsigned long long) st->xrep, (unsigned long long) st->varies, (unsigned long long) st->statchg,
				(unsigned long long) st->paths, (unsigned long long) st->objarg, (unsigned long long) st->objres, (unsigned long long) st->byref,
				(unsigned long long) st->big, (unsigned long long) st->out, (unsigned long long) st->err, (unsigned long long) st->exc,
				(unsigned long long) st->glob, (unsigned long long) st->distinct, (unsigned long long) st->argbytes);
		}
	} ZEND_HASH_FOREACH_END();
	fclose(o);
	snprintf(path, sizeof path, "%s.%d.sizes", mp_path, (int) getpid());
	if (!(o = fopen(path, "w"))) {
		return;
	}
	fprintf(o, "function\tcalls\tbytes\thits\tmisses\tmiss_newlen\tmiss_newfp\tlengths...\n");
	ZEND_HASH_FOREACH_PTR(&mp_stats, st) {
		if (st->sz && st->sz->n) {
			mp_sizes *z = st->sz;
			fprintf(o, "%s\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu", ZSTR_VAL(st->name), (unsigned long long) z->n, (unsigned long long) z->bytes,
				(unsigned long long) z->hits, (unsigned long long) z->misses, (unsigned long long) z->miss_newlen, (unsigned long long) z->miss_newfp);
			for (uint64_t i = 0; i < z->n; i++) {
				fprintf(o, "\t%u", z->len[i]);
			}
			fputc('\n', o);
		}
	} ZEND_HASH_FOREACH_END();
	fclose(o);
}

static void mp_reset(void)
{
	mp_stat *st;
	ZEND_HASH_FOREACH_PTR(&mp_stats, st) {
		zend_string *name = st->name;
		mp_sizes *z = st->sz;
		mp_stat *alt = st->alt;
		memset(st, 0, sizeof *st);
		st->name = name;
		st->alt = alt;
		if ((st->sz = z)) {
			z->n = z->bytes = z->hits = z->misses = z->miss_newlen = z->miss_newfp = 0;
			zend_hash_clean(&z->lens);
			zend_hash_clean(&z->fps);
		}
	} ZEND_HASH_FOREACH_END();
	memset(mp_tab, 0, mp_tabcap * sizeof(mp_seen));
	mp_tabn = mp_req = 0;
	mp_req_cpu = mp_req_wall = 0;
}

static void phasync_memo_profile_minit(void)
{
	const char *p = getenv("PHASYNC_MEMO_PROFILE");
	if (!p || !*p) {
		return;
	}
	snprintf(mp_path, sizeof mp_path, "%s", p);
	mp_on = true;
	mp_pid = getpid();
	for (int i = 0; i < 21000; i++) {
		uint64_t c = mp_clock(CLOCK_THREAD_CPUTIME_ID), w = mp_now(), w2 = mp_now(), c2 = mp_clock(CLOCK_THREAD_CPUTIME_ID);
		if (i >= 1000) {
			mp_cal_wall += w2 - w;
			mp_cal_cpu += c2 - c;
		}
	}
	mp_cal_wall /= 20000;
	mp_cal_cpu /= 20000;
	zend_hash_init(&mp_stats, 4096, NULL, NULL, 1);
	zend_hash_init(&mp_byfn, 4096, NULL, NULL, 1);
	zend_hash_init(&mp_fibers, 64, NULL, NULL, 1);
	zend_observer_fcall_register(mp_init);
	zend_observer_error_register(mp_error);
	zend_observer_fiber_switch_register(mp_fiber_switch);
	zend_observer_fiber_destroy_register(mp_fiber_destroy);
	mp_ub_write_orig = sapi_module.ub_write;
	sapi_module.ub_write = mp_ub_write;
}

static void phasync_memo_profile_rinit(void)
{
	if (mp_on) {
		mp_rcpu = mp_clock(CLOCK_THREAD_CPUTIME_ID);
		mp_rwall = mp_now();
	}
}

/* Post-deactivate: one request (one run_next() run) done */
static void phasync_memo_profile_request_end(void)
{
	mp_stack *s;
	if (!mp_on) {
		return;
	}
	mp_req_cpu += mp_clock(CLOCK_THREAD_CPUTIME_ID) - mp_rcpu;
	mp_req_wall += mp_now() - mp_rwall;
	mp_req++;
	if (getpid() != mp_pid) {   /* a forked worker: the parent's requests are not its own */
		mp_pid = getpid();
		mp_reset();
	}
	ZEND_HASH_FOREACH_PTR(&mp_fibers, s) {
		if (s != &mp_main) {
			free(s->f);
			free(s);
		}
	} ZEND_HASH_FOREACH_END();
	zend_hash_clean(&mp_fibers);
	mp_main.n = 0;
	mp_main.left = 0;
	mp_cur = &mp_main;
	if (mp_req % 50 == 0) {
		mp_dump();
	}
}

static void phasync_memo_profile_mshutdown(void)
{
	if (mp_on) {
		mp_dump();
		sapi_module.ub_write = mp_ub_write_orig;
		mp_on = false;
	}
}
