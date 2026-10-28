/* phasync.gc_grace: gc_collect_cycles() does not walk into objects younger than
 * the grace period (ms). Hiding an object's children can only make data look
 * alive, never free a live object.
 *
 * Age: gc_stamp[handle] is the time (ms since the request started, 31 bits,
 * compared modulo 2^31) of the first collection that saw the object. Each
 * outer collection stamps the slots past the previous scan's top and those std
 * free_obj marked PHASYNC_GC_FREED (listed on gc_freed) that hold an object again.
 * A slot freed by other handlers keeps its stamp: the new object looks older,
 * which only means it is not hidden. Every hidden object goes on gc_hidden once
 * (top bit of its stamp) and is put back in the root buffer when old, so its
 * garbage is still collected: the list is walked from its oldest end up to the
 * first one still young, so one may wait up to a grace longer.
 *
 * The handlers are patched per request (RINIT to RSHUTDOWN): everything is per
 * request and in its heap, and shutdown keeps PHP's fast free path. Collections
 * in the request's shutdown hide nothing. */

#include <sys/mman.h>
#include "Zend/zend_hrtime.h"

#define PHASYNC_GC_HIDDEN ((uint32_t) 1u << 31)
#define PHASYNC_GC_FREED  ((uint32_t) 0x7fffffffu)

static int (*phasync_gc_collect_prev)(void);
static const char *phasync_gc_off_reason;

/* gc_stamp..gc_st_ns: per request, zeroed at RINIT */
static struct {
	zend_long gc_grace;           /* INI phasync.gc_grace (ms), 0 = off */
	uint32_t *gc_stamp;           /* handle -> first seen (ms), PHASYNC_GC_FREED, top bit: on gc_hidden */
	uint32_t *gc_hidden;          /* handles hidden from a collection, until old (from gc_hidden_head) */
	uint32_t *gc_freed;           /* handles std free_obj marked PHASYNC_GC_FREED, below gc_prev_top */
	uint32_t gc_cap, gc_prev_top, gc_hidden_head, gc_hidden_n, gc_hidden_cap, gc_freed_n, gc_freed_cap, gc_now;
	bool gc_on, gc_in, gc_hide;
	zend_hrtime_t gc_start;
	uint64_t gc_st_collections, gc_st_hidden, gc_st_rerooted, gc_st_ns;
} phasync_gc_g;
#define PHASYNC_GC(v) (phasync_gc_g.v)

static bool phasync_gc_patch(zend_object_get_gc_t get_gc, zend_object_free_obj_t free_obj)
{
	long pg = sysconf(_SC_PAGESIZE);
	uintptr_t start = (uintptr_t) &std_object_handlers & ~(uintptr_t) (pg - 1);
	uintptr_t end = ((uintptr_t) (&std_object_handlers + 1) + pg - 1) & ~(uintptr_t) (pg - 1);
	zend_object_handlers *h = (zend_object_handlers *) &std_object_handlers;

	__asm__ volatile ("" : "+r" (h));   /* it is const: keep the stores */
	if (mprotect((void *) start, end - start, PROT_READ | PROT_WRITE) != 0) {
		return false;
	}
	h->get_gc = get_gc;
	h->free_obj = free_obj;
	mprotect((void *) start, end - start, PROT_READ);
	return true;
}

static zend_always_inline bool phasync_gc_young(uint32_t s)
{
	s &= ~PHASYNC_GC_HIDDEN;
	return s != PHASYNC_GC_FREED && ((PHASYNC_GC(gc_now) - s) & 0x7fffffffu) < (uint32_t) PHASYNC_GC(gc_grace);
}

static void phasync_gc_push(uint32_t **a, uint32_t *n, uint32_t *cap, uint32_t h)
{
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 256;
		*a = safe_erealloc(*a, *cap, sizeof(uint32_t), 0);
	}
	(*a)[(*n)++] = h;
}

static void phasync_gc_free_obj(zend_object *object)
{
	uint32_t h = object->handle;

	if (h < PHASYNC_GC(gc_cap) && PHASYNC_GC(gc_stamp)[h] != PHASYNC_GC_FREED) {
		PHASYNC_GC(gc_stamp)[h] = PHASYNC_GC_FREED;
		phasync_gc_push(&PHASYNC_GC(gc_freed), &PHASYNC_GC(gc_freed_n), &PHASYNC_GC(gc_freed_cap), h);
	}
	zend_object_std_dtor(object);
}

static HashTable *phasync_gc_get_gc(zend_object *object, zval **table, int *n)
{
	uint32_t h = object->handle, s;

	if (!PHASYNC_GC(gc_hide) || h >= PHASYNC_GC(gc_cap) || !phasync_gc_young(s = PHASYNC_GC(gc_stamp)[h])) {
		return zend_std_get_gc(object, table, n);
	}
	if (!(s & PHASYNC_GC_HIDDEN)) {
		PHASYNC_GC(gc_stamp)[h] = s | PHASYNC_GC_HIDDEN;
		phasync_gc_push(&PHASYNC_GC(gc_hidden), &PHASYNC_GC(gc_hidden_n), &PHASYNC_GC(gc_hidden_cap), h);
		PHASYNC_GC(gc_st_hidden)++;
	}
	*table = NULL;
	*n = 0;
	return NULL;
}

/* Stamp the objects new since the previous scan. */
static void phasync_gc_scan(void)
{
	zend_objects_store *os = &EG(objects_store);
	uint32_t top = os->top, prev = PHASYNC_GC(gc_prev_top);

	if (top > PHASYNC_GC(gc_cap)) {
		uint32_t cap = top + top / 2;

		PHASYNC_GC(gc_stamp) = safe_erealloc(PHASYNC_GC(gc_stamp), cap, sizeof(uint32_t), 0);
		for (uint32_t i = top; i < cap; i++) {
			PHASYNC_GC(gc_stamp)[i] = PHASYNC_GC_FREED;   /* objects a collection's destructors make there are new */
		}
		PHASYNC_GC(gc_cap) = cap;
	}
	uint32_t kept = 0;

	for (uint32_t k = 0; k < PHASYNC_GC(gc_freed_n); k++) {
		uint32_t h = PHASYNC_GC(gc_freed)[k];

		if (IS_OBJ_VALID(os->object_buckets[h])) {
			PHASYNC_GC(gc_stamp)[h] = PHASYNC_GC(gc_now);
		} else {
			PHASYNC_GC(gc_freed)[kept++] = h;
		}
	}
	PHASYNC_GC(gc_freed_n) = kept;
	for (uint32_t i = MAX(prev, 1); i < top; i++) {
		if (IS_OBJ_VALID(os->object_buckets[i])) {
			PHASYNC_GC(gc_stamp)[i] = PHASYNC_GC(gc_now);
		} else {
			PHASYNC_GC(gc_stamp)[i] = PHASYNC_GC_FREED;
			phasync_gc_push(&PHASYNC_GC(gc_freed), &PHASYNC_GC(gc_freed_n), &PHASYNC_GC(gc_freed_cap), i);
		}
	}
	PHASYNC_GC(gc_prev_top) = top;
}

/* Back in the root buffer: the hidden objects now old, or all of them. */
static void phasync_gc_reroot(bool all)
{
	uint32_t i;

	/* A collection gc_check_possible_root() starts appends here: re-read n */
	for (i = PHASYNC_GC(gc_hidden_head); i < PHASYNC_GC(gc_hidden_n); i++) {
		uint32_t h = PHASYNC_GC(gc_hidden)[i], s = PHASYNC_GC(gc_stamp)[h];
		zend_object *obj = EG(objects_store).object_buckets[h];

		if (!(s & PHASYNC_GC_HIDDEN)) {
			continue;   /* freed (and maybe reused) since */
		}
		if (!all && phasync_gc_young(s)) {
			break;
		}
		PHASYNC_GC(gc_stamp)[h] = s & ~PHASYNC_GC_HIDDEN;
		if (IS_OBJ_VALID(obj)) {
			PHASYNC_GC(gc_st_rerooted)++;
			gc_check_possible_root((zend_refcounted *) obj);
		}
	}
	if (i == PHASYNC_GC(gc_hidden_n)) {
		PHASYNC_GC(gc_hidden_head) = PHASYNC_GC(gc_hidden_n) = 0;
	} else if (i > PHASYNC_GC(gc_hidden_n) / 2) {
		memmove(PHASYNC_GC(gc_hidden), PHASYNC_GC(gc_hidden) + i, (PHASYNC_GC(gc_hidden_n) - i) * sizeof(uint32_t));
		PHASYNC_GC(gc_hidden_n) -= i;
		PHASYNC_GC(gc_hidden_head) = 0;
	} else {
		PHASYNC_GC(gc_hidden_head) = i;
	}
}

static int phasync_gc_collect(void)
{
	int freed;

	if (!PHASYNC_GC(gc_on) || PHASYNC_GC(gc_in)) {
		return phasync_gc_collect_prev();   /* nested: the outer call's scan stands */
	}
	{
		zend_hrtime_t t0 = zend_hrtime();
		uint32_t now = (uint32_t) ((t0 - PHASYNC_GC(gc_start)) / 1000000) & 0x7fffffffu;

		PHASYNC_GC(gc_in) = true;
		PHASYNC_GC(gc_now) = now == PHASYNC_GC_FREED ? now - 1 : now;
		PHASYNC_GC(gc_hide) = !(EG(flags) & EG_FLAGS_IN_SHUTDOWN);
		phasync_gc_scan();
		phasync_gc_reroot(!PHASYNC_GC(gc_hide));
		PHASYNC_GC(gc_st_collections)++;
		PHASYNC_GC(gc_st_ns) += zend_hrtime() - t0;
	}
	zend_try {
		freed = phasync_gc_collect_prev();
	} zend_catch {
		PHASYNC_GC(gc_in) = false;
		zend_bailout();
	} zend_end_try();
	PHASYNC_GC(gc_in) = false;
	return freed;
}

static void phasync_gc_minit(void)
{
	PHASYNC_GC(gc_grace) = INI_INT("phasync.gc_grace");
	if (PHASYNC_GC(gc_grace) <= 0) {
		PHASYNC_GC(gc_grace) = 0;
		return;
	}
	if (PHASYNC_GC(gc_grace) > 0x3fffffff) {
		PHASYNC_GC(gc_grace) = 0x3fffffff;
	}
	if (std_object_handlers.get_gc != zend_std_get_gc || std_object_handlers.free_obj != zend_object_std_dtor) {
		phasync_gc_off_reason = "std_object_handlers already replaced by another extension";
		PHASYNC_GC(gc_grace) = 0;
		return;
	}
	phasync_gc_collect_prev = gc_collect_cycles;
	gc_collect_cycles = phasync_gc_collect;
}

static void phasync_gc_rinit(void)
{
	memset(&PHASYNC_GC(gc_stamp), 0, (char *) &PHASYNC_GC(gc_st_ns) + sizeof(PHASYNC_GC(gc_st_ns)) - (char *) &PHASYNC_GC(gc_stamp));
	if (PHASYNC_GC(gc_grace)) {
		PHASYNC_GC(gc_start) = zend_hrtime();
		PHASYNC_GC(gc_on) = phasync_gc_patch(phasync_gc_get_gc, phasync_gc_free_obj);
		if (!PHASYNC_GC(gc_on)) {
			phasync_gc_off_reason = "mprotect() of std_object_handlers failed";
		}
	}
}

static void phasync_gc_rshutdown(void)
{
	if (PHASYNC_GC(gc_on)) {
		phasync_gc_patch(zend_std_get_gc, zend_object_std_dtor);
		PHASYNC_GC(gc_on) = false;
		if (PHASYNC_GC(gc_stamp)) {
			efree(PHASYNC_GC(gc_stamp));
		}
		if (PHASYNC_GC(gc_hidden)) {
			efree(PHASYNC_GC(gc_hidden));
		}
		if (PHASYNC_GC(gc_freed)) {
			efree(PHASYNC_GC(gc_freed));
		}
	}
}

ZEND_FUNCTION(phasync_ext_gc_grace_stats)
{
	ZEND_PARSE_PARAMETERS_NONE();

	array_init(return_value);
	add_assoc_long(return_value, "grace_ms", PHASYNC_GC(gc_on) ? PHASYNC_GC(gc_grace) : 0);
	add_assoc_long(return_value, "collections", (zend_long) PHASYNC_GC(gc_st_collections));
	add_assoc_long(return_value, "hidden", (zend_long) PHASYNC_GC(gc_st_hidden));
	add_assoc_long(return_value, "held", (zend_long) (PHASYNC_GC(gc_hidden_n) - PHASYNC_GC(gc_hidden_head)));
	add_assoc_long(return_value, "rerooted", (zend_long) PHASYNC_GC(gc_st_rerooted));
	add_assoc_long(return_value, "scan_ns", (zend_long) PHASYNC_GC(gc_st_ns));
}
