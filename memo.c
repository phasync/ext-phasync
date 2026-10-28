/* What a long-running process remembers across requests: the results of json_decode(),
 * unserialize(), file_get_contents(), scandir() and glob() (phasync.memo_memory; README, Memo).
 *
 * An entry is one malloc'd block holding its key and an immutable copy of the value, laid out as
 * opcache lays out literal arrays: arrays and strings flagged immutable and persistent, never
 * refcounted, so a hit hands the value out at no cost and PHP separates it on the first write.
 * stdClass objects cannot be immutable: object-mode results are kept as a template (an IS_PTR
 * node where an object, or an array holding one, goes) and handed out as fresh stdClass graphs,
 * sharing every object-free table and string.
 *
 * Something is stored the second time the process produces it. Least recently used entries go
 * past phasync.memo_memory, counted in the bytes their blocks take. A running request may still
 * hold an evicted value, so its block goes to a graveyard freed when the request ends; while the
 * graveyard holds memo_memory bytes (one request, a Swerve async worker's, evicting that much),
 * nothing new is stored until a request ends: the memo never takes more than twice
 * phasync.memo_memory.
 *
 * A json_decode() or unserialize() text is hashed only when its fingerprint (its length and the 64
 * bytes in its middle, where texts sharing an envelope differ) is one a stored text has, or when
 * its hash is cached already: most misses never pay for the hash. "Seen once" is tracked by
 * fingerprint too, so a text sharing one with a text seen before is stored on its first sight; a
 * hit still compares the whole text. */

#include "ext/json/php_json.h"
#include "Zend/zend_hrtime.h"

/* A decode faster than this is not worth a cache entry: a hit costs about as much */
#define PHASYNC_MEMO_MIN_NS 500

#define PHASYNC_MEMO_MAX_FILE (4 * 1024 * 1024)

typedef struct phasync_memo_entry {
	struct phasync_memo_entry *next;   /* in the graveyard */
	size_t bytes;                      /* the whole block */
	uint64_t used;                     /* when last handed out: the LRU order across the tables */
	zend_string *key;
	zend_ulong print;                  /* a text's fingerprint, 0 for files and listings */
	zval value;                        /* immutable, or IS_PTR to a phasync_memo_node */
} phasync_memo_entry;

/* An object, or an array holding objects: materialised on each hit */
typedef struct {
	HashTable *t;       /* the array, or the object's properties (NULL: none) */
	bool object, deep;  /* deep: t holds nodes */
} phasync_memo_node;

/* The file key: any write changes the size, mtime or ctime */
typedef struct {
	dev_t dev;
	ino_t ino;
	off_t size;
	struct timespec mtime, ctime;
	zend_long offset, length;   /* what was read: from offset, length bytes (-1: to the end) */
	uint64_t serial;            /* the stat cache's (statcache.c), 0 without it */
} phasync_memo_file_key;

/* Copying a request value into a block: run once to size the block (mem NULL), then to fill it */
typedef struct {
	char *mem;
	size_t size;
	HashTable strings;   /* content -> its copy: each distinct string once per block */
} phasync_memo_copy;

static HashTable phasync_memo_arrays, phasync_memo_objects, phasync_memo_files, phasync_memo_unserialized, phasync_memo_dirs;   /* key -> entry */
static HashTable phasync_memo_seen;    /* hashes of what this process produced once */
static HashTable phasync_memo_prints;  /* fingerprint -> stored texts having it */
static phasync_memo_entry *phasync_memo_graveyard;
static size_t phasync_memo_limit, phasync_memo_bytes, phasync_memo_grave_bytes;
static uint64_t phasync_memo_clock, phasync_memo_errors;
static uint64_t phasync_memo_json_hits, phasync_memo_file_hits, phasync_memo_unserialize_hits, phasync_memo_dir_hits;
static uint64_t phasync_memo_misses, phasync_memo_stored, phasync_memo_evicted, phasync_memo_refused;
static zif_handler phasync_memo_orig_json_decode, phasync_memo_orig_file_get_contents, phasync_memo_orig_unserialize, phasync_memo_orig_scandir, phasync_memo_orig_glob;
static bool phasync_memo_on;

static void *phasync_memo_alloc(phasync_memo_copy *c, size_t n)
{
	void *p = c->mem;

	n = ZEND_MM_ALIGNED_SIZE(n);
	c->size += n;
	if (p) {
		c->mem += n;
	}
	return p;
}

static zend_string *phasync_memo_str(phasync_memo_copy *c, zend_string *s)
{
	zend_string *copy;
	zval *found, p;

	if (ZSTR_LEN(s) <= 1) {
		return ZSTR_LEN(s) ? ZSTR_CHAR((zend_uchar) ZSTR_VAL(s)[0]) : ZSTR_EMPTY_ALLOC();
	}
	if ((found = zend_hash_find(&c->strings, s))) {
		return Z_PTR_P(found);
	}
	if ((copy = phasync_memo_alloc(c, _ZSTR_STRUCT_SIZE(ZSTR_LEN(s))))) {
		GC_SET_REFCOUNT(copy, 1);
		GC_TYPE_INFO(copy) = GC_STRING | ((IS_STR_INTERNED | IS_STR_PERSISTENT | IS_STR_PERMANENT | (GC_FLAGS(s) & IS_STR_VALID_UTF8)) << GC_FLAGS_SHIFT);
		ZSTR_H(copy) = zend_string_hash_val(s);
		ZSTR_LEN(copy) = ZSTR_LEN(s);
		memcpy(ZSTR_VAL(copy), ZSTR_VAL(s), ZSTR_LEN(s) + 1);
	}
	ZVAL_PTR(&p, copy);
	zend_hash_add_new(&c->strings, s, &p);
	return copy;
}

static bool phasync_memo_zval(phasync_memo_copy *c, zval *dst, zval *src);

/* An immutable copy of t; *deep: whether it holds objects */
static HashTable *phasync_memo_array(phasync_memo_copy *c, HashTable *t, bool *deep)
{
	size_t data = HT_IS_PACKED(t) ? HT_PACKED_USED_SIZE(t) : HT_USED_SIZE(t);
	HashTable *copy;
	char *mem;

	*deep = false;
	if (!zend_hash_num_elements(t)) {
		return (HashTable *) &zend_empty_array;
	}
	copy = phasync_memo_alloc(c, sizeof(HashTable));
	mem = phasync_memo_alloc(c, data);
	if (copy) {
		memcpy(copy, t, sizeof(HashTable));
		memcpy(mem, HT_GET_DATA_ADDR(t), data);
		HT_SET_DATA_ADDR(copy, mem);
		GC_SET_REFCOUNT(copy, 2);
		GC_TYPE_INFO(copy) = GC_ARRAY | ((IS_ARRAY_IMMUTABLE | IS_ARRAY_PERSISTENT | GC_NOT_COLLECTABLE) << GC_FLAGS_SHIFT);
		HT_FLAGS(copy) |= HASH_FLAG_STATIC_KEYS;
		copy->pDestructor = NULL;
		copy->nInternalPointer = 0;
	}
	for (uint32_t i = 0; i < t->nNumUsed; i++) {
		zval *v = HT_IS_PACKED(t) ? &t->arPacked[i] : &t->arData[i].val;
		zval *d = !copy ? NULL : HT_IS_PACKED(t) ? &copy->arPacked[i] : &copy->arData[i].val;
		zend_string *k;

		if (Z_TYPE_P(v) == IS_UNDEF) {
			continue;
		}
		if (!HT_IS_PACKED(t) && t->arData[i].key) {
			k = phasync_memo_str(c, t->arData[i].key);
			if (copy) {
				copy->arData[i].key = k;
			}
		}
		ZEND_ASSERT(Z_TYPE_P(v) <= IS_OBJECT);   /* no references or resources: the callers check */
		if (Z_TYPE_P(v) >= IS_STRING) {
			*deep |= phasync_memo_zval(c, d, v);
		}
	}
	return copy;
}

/* dst (NULL while sizing): src's immutable copy, or a node (returns true). Leaves dst's u2 (a bucket's chain) alone. */
static bool phasync_memo_zval(phasync_memo_copy *c, zval *dst, zval *src)
{
	phasync_memo_node *n;
	HashTable *t;
	bool deep;

	switch (Z_TYPE_P(src)) {
		case IS_STRING: {
			zend_string *s = phasync_memo_str(c, Z_STR_P(src));
			if (dst) {
				Z_STR_P(dst) = s;
				Z_TYPE_INFO_P(dst) = IS_INTERNED_STRING_EX;
			}
			return false;
		}
		case IS_ARRAY:
			t = phasync_memo_array(c, Z_ARR_P(src), &deep);
			if (!deep) {
				if (dst) {
					Z_ARR_P(dst) = t;
					Z_TYPE_INFO_P(dst) = IS_ARRAY;   /* not refcounted: immutable */
				}
				return false;
			}
			n = phasync_memo_alloc(c, sizeof *n);
			if (n) {
				n->t = t;
				n->object = false;
				n->deep = true;
			}
			break;
		default:   /* IS_OBJECT: a stdClass (the callers check) */
			t = NULL;
			deep = false;
			if (Z_OBJ_P(src)->properties && zend_hash_num_elements(Z_OBJ_P(src)->properties)) {
				t = phasync_memo_array(c, Z_OBJ_P(src)->properties, &deep);
			}
			n = phasync_memo_alloc(c, sizeof *n);
			if (n) {
				n->t = t;
				n->object = true;
				n->deep = deep;
			}
	}
	if (dst) {
		Z_PTR_P(dst) = n;
		Z_TYPE_INFO_P(dst) = IS_PTR;
	}
	return true;
}

/* Hands out a value: immutable as it is, a node as new objects and arrays */
static void phasync_memo_out(zval *dst, zval *src)
{
	phasync_memo_node *n;

	if (Z_TYPE_P(src) != IS_PTR) {
		ZVAL_COPY_VALUE(dst, src);
		return;
	}
	n = Z_PTR_P(src);
	if (n->object) {
		zend_object *o = zend_objects_new(zend_standard_class_def);
		HashTable *props = n->t;

		if (props && n->deep) {
			zval *v;
			props = zend_array_dup(props);
			ZEND_HASH_FOREACH_VAL(props, v) {
				if (Z_TYPE_P(v) == IS_PTR) {
					phasync_memo_out(v, v);
				}
			} ZEND_HASH_FOREACH_END();
		}
		o->properties = props;   /* an immutable one is separated on the first write */
		ZVAL_OBJ(dst, o);
	} else {
		HashTable *t = zend_array_dup(n->t);
		zval *v;
		ZEND_HASH_FOREACH_VAL(t, v) {
			if (Z_TYPE_P(v) == IS_PTR) {
				phasync_memo_out(v, v);
			}
		} ZEND_HASH_FOREACH_END();
		ZVAL_ARR(dst, t);
	}
}

/* A table's destructor: the block waits for the request's end, where nothing can hold it */
static void phasync_memo_entry_dtor(zval *zv)
{
	phasync_memo_entry *e = Z_PTR_P(zv);

	if (e->print && !--Z_LVAL_P(zend_hash_index_find(&phasync_memo_prints, e->print))) {
		zend_hash_index_del(&phasync_memo_prints, e->print);
	}
	phasync_memo_bytes -= e->bytes;
	phasync_memo_grave_bytes += e->bytes;
	e->next = phasync_memo_graveyard;
	phasync_memo_graveyard = e;
}

static void phasync_memo_bury(void)
{
	while (phasync_memo_graveyard) {
		phasync_memo_entry *e = phasync_memo_graveyard;
		phasync_memo_graveyard = e->next;
		pefree(e, 1);
	}
	phasync_memo_grave_bytes = 0;
}

/* Evicts least recently used entries until `bytes` more fit; false when the graveyard cannot take them */
static bool phasync_memo_make_room(size_t bytes)
{
	HashTable *tables[5] = { &phasync_memo_arrays, &phasync_memo_objects, &phasync_memo_files, &phasync_memo_unserialized, &phasync_memo_dirs };

	while (phasync_memo_bytes + bytes > phasync_memo_limit) {
		HashTable *oldest = NULL;
		phasync_memo_entry *e = NULL;

		for (int i = 0; i < 5; i++) {
			zval *first;
			HashPosition pos;
			zend_hash_internal_pointer_reset_ex(tables[i], &pos);
			if ((first = zend_hash_get_current_data_ex(tables[i], &pos)) && (!e || ((phasync_memo_entry *) Z_PTR_P(first))->used < e->used)) {
				e = Z_PTR_P(first);
				oldest = tables[i];
			}
		}
		if (phasync_memo_grave_bytes + e->bytes > phasync_memo_limit) {
			return false;
		}
		zend_hash_del(oldest, e->key);
		phasync_memo_evicted++;
	}
	return true;
}

/* An entry just handed out becomes the most recently used: moved to its table's end */
static phasync_memo_entry *phasync_memo_touch(HashTable *t, zval *found)
{
	phasync_memo_entry *e = Z_PTR_P(found);
	zval keep;

	e->used = ++phasync_memo_clock;
	if (Z_PTR(t->arData[t->nNumUsed - 1].val) == e && Z_TYPE(t->arData[t->nNumUsed - 1].val) != IS_UNDEF) {
		return e;   /* already last */
	}
	ZVAL_PTR(&keep, e);
	t->pDestructor = NULL;   /* moved, not destroyed */
	zend_hash_del(t, e->key);
	t->pDestructor = phasync_memo_entry_dtor;
	zend_hash_add_new(t, e->key, &keep);
	return e;
}

/* Stores value under key: a block holding the entry, the key and the value's immutable copy */
static void phasync_memo_put(HashTable *t, zend_string *key, zend_ulong print, zval *value)
{
	phasync_memo_copy c;
	phasync_memo_entry *e;
	size_t size;
	zval p;

	if (zend_hash_exists(t, key)) {
		return;   /* another fiber stored it while this one waited */
	}
	zend_hash_init(&c.strings, 16, NULL, NULL, 0);
	c.mem = NULL;
	c.size = ZEND_MM_ALIGNED_SIZE(sizeof *e);
	phasync_memo_str(&c, key);
	phasync_memo_zval(&c, NULL, value);
	size = c.size;
	if (size > phasync_memo_limit / 4 || !phasync_memo_make_room(size)) {
		phasync_memo_refused++;
		zend_hash_destroy(&c.strings);
		return;
	}
	zend_hash_clean(&c.strings);
	e = pemalloc(size, 1);
	c.mem = (char *) e;
	c.size = 0;
	phasync_memo_alloc(&c, sizeof *e);
	e->key = phasync_memo_str(&c, key);
	phasync_memo_zval(&c, &e->value, value);
	ZEND_ASSERT(c.size == size);
	zend_hash_destroy(&c.strings);
	e->bytes = size;
	if ((e->print = print)) {
		zval *n = zend_hash_index_lookup(&phasync_memo_prints, print);   /* IS_NULL when new */
		ZVAL_LONG(n, Z_TYPE_P(n) == IS_NULL ? 1 : Z_LVAL_P(n) + 1);
	}
	e->used = ++phasync_memo_clock;
	phasync_memo_bytes += size;
	phasync_memo_stored++;
	ZVAL_PTR(&p, e);
	zend_hash_add_new(t, e->key, &p);
}

/* A text's fingerprint: its length and the 64 bytes in its middle (all of it when shorter); never 0 */
static zend_ulong phasync_memo_print(zend_string *s)
{
	size_t n = ZSTR_LEN(s), m = MIN(n, 64);
	const char *p = ZSTR_VAL(s) + (n - m) / 2;
	uint64_t h = n, w;

	for (; m >= 8; m -= 8, p += 8) {
		memcpy(&w, p, 8);
		h = (h ^ w) * 0x9E3779B97F4A7C15ULL;
	}
	if (m) {
		w = 0;
		memcpy(&w, p, m);
		h = (h ^ w) * 0x9E3779B97F4A7C15ULL;
	}
	return (zend_ulong) (h ^ h >> 29) | 1;
}

/* The text's entry, looked up only when its hash is cached (free) or its fingerprint is a stored text's */
static zval *phasync_memo_find_text(HashTable *t, zend_string *s, zend_ulong *print)
{
	if (!ZSTR_H(s) && !zend_hash_index_exists(&phasync_memo_prints, *print = phasync_memo_print(s))) {
		return NULL;
	}
	return zend_hash_find(t, s);
}

/* Stored the second time this process produces it */
static bool phasync_memo_second_time(zend_ulong h)
{
	if (zend_hash_index_exists(&phasync_memo_seen, h)) {
		return true;
	}
	if (zend_hash_num_elements(&phasync_memo_seen) >= 4096) {
		zend_hash_clean(&phasync_memo_seen);
	}
	zend_hash_index_add_empty_element(&phasync_memo_seen, h);
	return false;
}

/* Every error, warning, notice or deprecation: a call that raised one is not stored, so a hit is
 * never a call that would have said something */
static void phasync_memo_error_observer(int type, zend_string *file, uint32_t line, zend_string *message)
{
	(void) type; (void) file; (void) line; (void) message;
	phasync_memo_errors++;
}

/* A value holding nothing but arrays, scalars and stdClass objects, each object once: no other
 * class (whose __wakeup()/__unserialize() runs code), no references, no object twice (which
 * per-hit copies would split in two). *objects: whether any stdClass was met. */
static bool phasync_memo_plain(zval *zv, HashTable *seen, bool *objects)
{
	HashTable *t;
	zval *v;

	switch (Z_TYPE_P(zv)) {
		case IS_OBJECT:
			if (Z_OBJCE_P(zv) != zend_standard_class_def || !zend_hash_index_add_empty_element(seen, Z_OBJ_HANDLE_P(zv))) {
				return false;
			}
			*objects = true;
			if (!(t = Z_OBJ_P(zv)->properties)) {
				return true;
			}
			break;
		case IS_ARRAY:
			t = Z_ARR_P(zv);
			break;
		case IS_REFERENCE:
		case IS_RESOURCE:
		case IS_INDIRECT:
			return false;
		default:
			return true;
	}
	ZEND_HASH_FOREACH_VAL(t, v) {
		if (!phasync_memo_plain(v, seen, objects)) {
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	return true;
}

/* json_decode($json[, $assoc[, 512[, 0]]]) */
static ZEND_NAMED_FUNCTION(phasync_memo_json_decode)
{
	uint32_t argc = ZEND_NUM_ARGS();
	zval *text = argc >= 1 ? ZEND_CALL_ARG(execute_data, 1) : NULL;
	zval *assoc = argc >= 2 ? ZEND_CALL_ARG(execute_data, 2) : NULL;
	zval *depth = argc >= 3 ? ZEND_CALL_ARG(execute_data, 3) : NULL;
	zval *flags = argc >= 4 ? ZEND_CALL_ARG(execute_data, 4) : NULL;
	uint64_t errors = phasync_memo_errors;
	HashTable *t;
	zval *found;
	zend_ulong print = 0;

	if (argc > 4 || !text || Z_TYPE_P(text) != IS_STRING
	 || (assoc && Z_TYPE_P(assoc) != IS_TRUE && Z_TYPE_P(assoc) != IS_FALSE && Z_TYPE_P(assoc) != IS_NULL)
	 || (depth && (Z_TYPE_P(depth) != IS_LONG || Z_LVAL_P(depth) != PHP_JSON_PARSER_DEFAULT_DEPTH))
	 || (flags && (Z_TYPE_P(flags) != IS_LONG || Z_LVAL_P(flags) != 0))) {
		phasync_memo_orig_json_decode(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	t = assoc && Z_TYPE_P(assoc) == IS_TRUE ? &phasync_memo_arrays : &phasync_memo_objects;
	if ((found = phasync_memo_find_text(t, Z_STR_P(text), &print))) {
		phasync_memo_json_hits++;
		JSON_G(error_code) = PHP_JSON_ERROR_NONE;
		phasync_memo_out(return_value, &phasync_memo_touch(t, found)->value);
		return;
	}
	phasync_memo_misses++;
	zend_hrtime_t t0 = zend_hrtime();
	phasync_memo_orig_json_decode(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	if (zend_hrtime() - t0 >= PHASYNC_MEMO_MIN_NS
	 && !EG(exception) && errors == phasync_memo_errors && (Z_TYPE_P(return_value) == IS_ARRAY || Z_TYPE_P(return_value) == IS_OBJECT)
	 && phasync_memo_second_time((print = print ? print : phasync_memo_print(Z_STR_P(text))) ^ (t == &phasync_memo_arrays))) {
		phasync_memo_put(t, Z_STR_P(text), print, return_value);
	}
}

static bool phasync_memo_file_stat(const char *path, phasync_memo_file_key *key)
{
	struct stat st;
	uint64_t serial;

	if (phasync_sc_stat(path, &st, &serial) != 0 || !S_ISREG(st.st_mode) || st.st_size > PHASYNC_MEMO_MAX_FILE) {
		return false;
	}
	memset(key, 0, sizeof *key);   /* compared as bytes: no padding left undefined */
	key->serial = serial;
	key->dev = st.st_dev;
	key->ino = st.st_ino;
	key->size = st.st_size;
	key->mtime = PHASYNC_ST_MTIM(st);
	key->ctime = PHASYNC_ST_CTIM(st);
	return true;
}

/* file_get_contents($path), or with $use_include_path false, no context and an $offset (not from
 * the end) and/or a $length: a plain local path */
static ZEND_NAMED_FUNCTION(phasync_memo_file_get_contents)
{
	uint32_t argc = ZEND_NUM_ARGS();
	zval *path = argc >= 1 ? ZEND_CALL_ARG(execute_data, 1) : NULL;
	zval *incl = argc >= 2 ? ZEND_CALL_ARG(execute_data, 2) : NULL, *ctx = argc >= 3 ? ZEND_CALL_ARG(execute_data, 3) : NULL;
	zval *off = argc >= 4 ? ZEND_CALL_ARG(execute_data, 4) : NULL, *len = argc >= 5 ? ZEND_CALL_ARG(execute_data, 5) : NULL;
	uint64_t errors = phasync_memo_errors;
	phasync_memo_file_key key, after;
	zend_string *k;
	zval *found;
	size_t expect;

	if (argc > 5 || !path || Z_TYPE_P(path) != IS_STRING || strlen(Z_STRVAL_P(path)) != Z_STRLEN_P(path) || strstr(Z_STRVAL_P(path), "://")
	 || (incl && Z_TYPE_P(incl) != IS_FALSE && Z_TYPE_P(incl) != IS_NULL) || (ctx && Z_TYPE_P(ctx) != IS_NULL)
	 || (off && (Z_TYPE_P(off) != IS_LONG || Z_LVAL_P(off) < 0)) || (len && Z_TYPE_P(len) != IS_NULL && (Z_TYPE_P(len) != IS_LONG || Z_LVAL_P(len) < 0))
	 || (PG(open_basedir) && *PG(open_basedir)) || !phasync_memo_file_stat(Z_STRVAL_P(path), &key)) {
		phasync_memo_orig_file_get_contents(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	key.offset = off ? Z_LVAL_P(off) : 0;
	key.length = len && Z_TYPE_P(len) == IS_LONG ? Z_LVAL_P(len) : -1;
	if (key.offset > key.size) {
		phasync_memo_orig_file_get_contents(INTERNAL_FUNCTION_PARAM_PASSTHRU);   /* PHP's own error */
		return;
	}
	expect = key.size - key.offset;
	if (key.length >= 0 && (size_t) key.length < expect) {
		expect = key.length;
	}
	k = zend_string_init((const char *) &key, sizeof key, 0);
	if ((found = zend_hash_find(&phasync_memo_files, k))) {
		phasync_memo_file_hits++;
		phasync_memo_out(return_value, &phasync_memo_touch(&phasync_memo_files, found)->value);
		zend_string_release(k);
		return;
	}
	phasync_memo_orig_file_get_contents(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	/* stored only when the file was the same before and after the read */
	if (Z_TYPE_P(return_value) == IS_STRING && Z_STRLEN_P(return_value) == expect && !EG(exception) && errors == phasync_memo_errors
	 && phasync_memo_file_stat(Z_STRVAL_P(path), &after) && (after.offset = key.offset, after.length = key.length, !memcmp(&key, &after, sizeof key))
	 && phasync_memo_second_time(zend_string_hash_val(k))) {
		phasync_memo_put(&phasync_memo_files, k, 0, return_value);
	}
	zend_string_release(k);
}

/* Whether a file system's mtimes tell when a directory changed: not /proc, /sys and the other
 * kernel file systems, whose listings change with no mtime change (/proc/self/task). Asked once
 * per device. */
static bool phasync_memo_dev_listable(dev_t dev, const char *dir)
{
	static HashTable verdicts;
	struct statfs fs;
	zval *v, b;

	if (!verdicts.nTableSize) {
		zend_hash_init(&verdicts, 8, NULL, NULL, 1);
	}
	if ((v = zend_hash_index_find(&verdicts, (zend_ulong) dev))) {
		return Z_TYPE_P(v) == IS_TRUE;
	}
	if (statfs(dir, &fs) != 0) {
		return false;
	}
#ifdef __linux__
	switch ((unsigned long) fs.f_type) {
	case 0x9fa0:      /* proc */
	case 0x62656572:  /* sysfs */
	case 0x27e0eb:    /* cgroup */
	case 0x63677270:  /* cgroup2 */
	case 0x64626720:  /* debugfs */
	case 0x74726163:  /* tracefs */
	case 0x73636673:  /* securityfs */
	case 0x1cd1:      /* devpts */
	case 0xcafe4a11:  /* bpf */
		ZVAL_FALSE(&b);
		break;
	default:
		ZVAL_TRUE(&b);
	}
#else
	ZVAL_BOOL(&b, strcmp(fs.f_fstypename, "devfs") != 0);   /* the BSD/macOS one */
#endif
	zend_hash_index_add_new(&verdicts, (zend_ulong) dev, &b);
	return Z_TYPE(b) == IS_TRUE;
}

/* A listing's key: the directory's stat, what lists it ('s' scandir, 'g' glob), a number (the
 * order or the flags) and the pattern; NULL when dir is not a directory */
static zend_string *phasync_memo_dir_key(const char *dir, char kind, zend_long n, const char *pattern, size_t plen)
{
	struct stat st;
	uint64_t serial;
	phasync_memo_file_key d;
	zend_string *k;

	if (phasync_sc_stat(dir, &st, &serial) != 0 || !S_ISDIR(st.st_mode) || !phasync_memo_dev_listable(st.st_dev, dir)) {
		return NULL;
	}
	memset(&d, 0, sizeof d);
	d.serial = serial;
	d.dev = st.st_dev;
	d.ino = st.st_ino;
	d.size = st.st_size;
	d.mtime = PHASYNC_ST_MTIM(st);
	d.ctime = PHASYNC_ST_CTIM(st);
	d.offset = kind;
	d.length = n;
	k = zend_string_alloc(sizeof d + plen, 0);
	memcpy(ZSTR_VAL(k), &d, sizeof d);
	memcpy(ZSTR_VAL(k) + sizeof d, pattern, plen);
	ZSTR_VAL(k)[sizeof d + plen] = '\0';
	return k;
}

/* A listing: from the memo, or listed and then stored when the directory was the same after */
static void phasync_memo_listing(INTERNAL_FUNCTION_PARAMETERS, zif_handler orig, const char *dir, char kind, zend_long n, const char *pattern, size_t plen)
{
	zend_string *k = phasync_memo_dir_key(dir, kind, n, pattern, plen), *after;
	uint64_t errors = phasync_memo_errors;
	zval *found;

	if (!k) {
		orig(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	if ((found = zend_hash_find(&phasync_memo_dirs, k))) {
		phasync_memo_dir_hits++;
		phasync_memo_out(return_value, &phasync_memo_touch(&phasync_memo_dirs, found)->value);
		zend_string_release(k);
		return;
	}
	orig(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	if (Z_TYPE_P(return_value) == IS_ARRAY && !EG(exception) && errors == phasync_memo_errors
	 && (after = phasync_memo_dir_key(dir, kind, n, pattern, plen))) {
		if (zend_string_equals(k, after) && phasync_memo_second_time(zend_string_hash_val(k))) {
			phasync_memo_put(&phasync_memo_dirs, k, 0, return_value);
		}
		zend_string_release(after);
	}
	zend_string_release(k);
}

/* scandir($dir[, $order]), no context */
static ZEND_NAMED_FUNCTION(phasync_memo_scandir)
{
	uint32_t argc = ZEND_NUM_ARGS();
	zval *dir = argc >= 1 ? ZEND_CALL_ARG(execute_data, 1) : NULL, *order = argc >= 2 ? ZEND_CALL_ARG(execute_data, 2) : NULL;

	if (argc > 2 || !dir || Z_TYPE_P(dir) != IS_STRING || strlen(Z_STRVAL_P(dir)) != Z_STRLEN_P(dir) || strstr(Z_STRVAL_P(dir), "://")
	 || (order && Z_TYPE_P(order) != IS_LONG) || (PG(open_basedir) && *PG(open_basedir))) {
		phasync_memo_orig_scandir(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	phasync_memo_listing(INTERNAL_FUNCTION_PARAM_PASSTHRU, phasync_memo_orig_scandir, Z_STRVAL_P(dir), 's', order ? Z_LVAL_P(order) : 0, "", 0);
}

/* glob($pattern[, $flags]) whose wildcards are all in its last path component */
static ZEND_NAMED_FUNCTION(phasync_memo_glob)
{
	uint32_t argc = ZEND_NUM_ARGS();
	zval *pattern = argc >= 1 ? ZEND_CALL_ARG(execute_data, 1) : NULL, *flags = argc >= 2 ? ZEND_CALL_ARG(execute_data, 2) : NULL;
	const char *p, *slash;
	char dir[MAXPATHLEN];
	size_t dlen;

	if (argc > 2 || !pattern || Z_TYPE_P(pattern) != IS_STRING || strlen(Z_STRVAL_P(pattern)) != Z_STRLEN_P(pattern)
	 || strstr(Z_STRVAL_P(pattern), "://") || (flags && Z_TYPE_P(flags) != IS_LONG) || (PG(open_basedir) && *PG(open_basedir))) {
		phasync_memo_orig_glob(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	p = Z_STRVAL_P(pattern);
	slash = strrchr(p, '/');
	dlen = slash ? (size_t) (slash - p) : 0;
	if (strcspn(p, "*?[{") < dlen || !strpbrk(slash ? slash : p, "*?[{") || dlen >= sizeof dir) {
		phasync_memo_orig_glob(INTERNAL_FUNCTION_PARAM_PASSTHRU);   /* wildcards before the last slash, or none */
		return;
	}
	if (slash) {
		memcpy(dir, p, dlen ? dlen : 1);   /* "/x*": the root */
		dir[dlen ? dlen : 1] = '\0';
	} else {
		dir[0] = '.';
		dir[1] = '\0';
	}
	phasync_memo_listing(INTERNAL_FUNCTION_PARAM_PASSTHRU, phasync_memo_orig_glob, dir, 'g', flags ? Z_LVAL_P(flags) : 0, p, Z_STRLEN_P(pattern));
}

/* unserialize($data), or unserialize($data, ['allowed_classes' => ...]): other options change
 * whether a text succeeds (max_depth), so a call giving them is left alone. allowed_classes decides
 * what a stdClass becomes, so a result holding one is stored and handed out only without options. */
static ZEND_NAMED_FUNCTION(phasync_memo_unserialize)
{
	uint32_t argc = ZEND_NUM_ARGS();
	zval *data = argc >= 1 ? ZEND_CALL_ARG(execute_data, 1) : NULL;
	zval *options = argc >= 2 ? ZEND_CALL_ARG(execute_data, 2) : NULL;
	uint64_t errors = phasync_memo_errors;
	zval *found;
	zend_ulong print = 0;

	if (options && Z_TYPE_P(options) == IS_ARRAY && !zend_hash_num_elements(Z_ARRVAL_P(options))) {
		options = NULL;   /* the default */
	}
	if (argc > 2 || !data || Z_TYPE_P(data) != IS_STRING
	 || (options && (Z_TYPE_P(options) != IS_ARRAY || zend_hash_num_elements(Z_ARRVAL_P(options)) != 1
	  || !zend_hash_str_exists(Z_ARRVAL_P(options), ZEND_STRL("allowed_classes"))))) {
		phasync_memo_orig_unserialize(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	if ((found = phasync_memo_find_text(&phasync_memo_unserialized, Z_STR_P(data), &print))
	 && (!options || Z_TYPE(((phasync_memo_entry *) Z_PTR_P(found))->value) != IS_PTR)) {
		phasync_memo_unserialize_hits++;
		phasync_memo_out(return_value, &phasync_memo_touch(&phasync_memo_unserialized, found)->value);
		return;
	}
	phasync_memo_misses++;
	zend_hrtime_t t0 = zend_hrtime();
	phasync_memo_orig_unserialize(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	if (zend_hrtime() - t0 >= PHASYNC_MEMO_MIN_NS
	 && (Z_TYPE_P(return_value) == IS_ARRAY || Z_TYPE_P(return_value) == IS_OBJECT) && !EG(exception) && errors == phasync_memo_errors
	 && phasync_memo_second_time((print = print ? print : phasync_memo_print(Z_STR_P(data))) ^ 2)) {
		HashTable seen;
		bool objects = false, plain;

		zend_hash_init(&seen, 8, NULL, NULL, 0);
		plain = phasync_memo_plain(return_value, &seen, &objects);
		zend_hash_destroy(&seen);
		if (plain && (!objects || !options)) {
			phasync_memo_put(&phasync_memo_unserialized, Z_STR_P(data), print, return_value);
		}
	}
}

/* MINIT: take over the five functions; phasync.memo_memory=0 leaves them alone */
static void phasync_memo_startup(void)
{
	static const char *const names[5] = { "json_decode", "file_get_contents", "unserialize", "scandir", "glob" };
	static zif_handler *const orig[5] = { &phasync_memo_orig_json_decode, &phasync_memo_orig_file_get_contents,
		&phasync_memo_orig_unserialize, &phasync_memo_orig_scandir, &phasync_memo_orig_glob };
	static const zif_handler mine[5] = { phasync_memo_json_decode, phasync_memo_file_get_contents,
		phasync_memo_unserialize, phasync_memo_scandir, phasync_memo_glob };
	zend_internal_function *f[5];
	zend_long size = zend_ini_parse_quantity_warn(zend_ini_str(ZEND_STRL("phasync.memo_memory"), false), zend_string_init_interned(ZEND_STRL("phasync.memo_memory"), 1));

	if (size <= 0) {
		return;
	}
	for (int i = 0; i < 5; i++) {
		if (!(f[i] = zend_hash_str_find_ptr(CG(function_table), names[i], strlen(names[i])))) {
			return;
		}
	}
	phasync_memo_limit = size;
	phasync_memo_on = true;
	zend_hash_init(&phasync_memo_arrays, 64, NULL, phasync_memo_entry_dtor, 1);
	zend_hash_init(&phasync_memo_objects, 64, NULL, phasync_memo_entry_dtor, 1);
	zend_hash_init(&phasync_memo_files, 64, NULL, phasync_memo_entry_dtor, 1);
	zend_hash_init(&phasync_memo_unserialized, 64, NULL, phasync_memo_entry_dtor, 1);
	zend_hash_init(&phasync_memo_dirs, 64, NULL, phasync_memo_entry_dtor, 1);
	zend_hash_init(&phasync_memo_seen, 64, NULL, NULL, 1);
	zend_hash_init(&phasync_memo_prints, 64, NULL, NULL, 1);
	zend_observer_error_register(phasync_memo_error_observer);
	for (int i = 0; i < 5; i++) {
		*orig[i] = f[i]->handler;
		f[i]->handler = mine[i];
	}
}

/* After the request's end (post-deactivate: every object and variable is gone) */
static void phasync_memo_request_end(void)
{
	if (phasync_memo_on) {
		phasync_memo_bury();
	}
}

static void phasync_memo_shutdown(void)
{
	static const char *const names[5] = { "json_decode", "file_get_contents", "unserialize", "scandir", "glob" };
	zif_handler orig[5] = { phasync_memo_orig_json_decode, phasync_memo_orig_file_get_contents,
		phasync_memo_orig_unserialize, phasync_memo_orig_scandir, phasync_memo_orig_glob };

	if (!phasync_memo_on) {
		return;
	}
	for (int i = 0; i < 5; i++) {
		((zend_internal_function *) zend_hash_str_find_ptr(CG(function_table), names[i], strlen(names[i])))->handler = orig[i];
	}
	zend_hash_destroy(&phasync_memo_arrays);
	zend_hash_destroy(&phasync_memo_objects);
	zend_hash_destroy(&phasync_memo_files);
	zend_hash_destroy(&phasync_memo_unserialized);
	zend_hash_destroy(&phasync_memo_dirs);
	zend_hash_destroy(&phasync_memo_seen);
	zend_hash_destroy(&phasync_memo_prints);
	phasync_memo_bury();
	phasync_memo_on = false;
}

ZEND_FUNCTION(phasync_ext_memo_stats)
{
	ZEND_PARSE_PARAMETERS_NONE();

	array_init(return_value);
	add_assoc_bool(return_value, "enabled", phasync_memo_on);
	add_assoc_long(return_value, "json_hits", (zend_long) phasync_memo_json_hits);
	add_assoc_long(return_value, "file_hits", (zend_long) phasync_memo_file_hits);
	add_assoc_long(return_value, "unserialize_hits", (zend_long) phasync_memo_unserialize_hits);
	add_assoc_long(return_value, "dir_hits", (zend_long) phasync_memo_dir_hits);
	add_assoc_long(return_value, "misses", (zend_long) phasync_memo_misses);
	add_assoc_long(return_value, "stored", (zend_long) phasync_memo_stored);
	add_assoc_long(return_value, "evicted", (zend_long) phasync_memo_evicted);
	add_assoc_long(return_value, "refused", (zend_long) phasync_memo_refused);
	add_assoc_long(return_value, "entries", phasync_memo_on ? (zend_long) (zend_hash_num_elements(&phasync_memo_arrays) + zend_hash_num_elements(&phasync_memo_objects)
		+ zend_hash_num_elements(&phasync_memo_files) + zend_hash_num_elements(&phasync_memo_unserialized) + zend_hash_num_elements(&phasync_memo_dirs)) : 0);
	add_assoc_long(return_value, "bytes", (zend_long) phasync_memo_bytes);
	add_assoc_long(return_value, "graveyard_bytes", (zend_long) phasync_memo_grave_bytes);
	add_assoc_long(return_value, "limit", (zend_long) phasync_memo_limit);
}
