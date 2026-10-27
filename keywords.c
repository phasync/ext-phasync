/* Included by phasync.c (single translation unit); not compiled on its own. */

/* run, begin and finish (phasync.keywords=1): a token rewrite before the compiler.
 *
 *     $r = run function () {...};   ->  $r = \phasync::run(function () {...});
 *     $f = begin fn() => work($m);  ->  $f = \phasync::go(fn() => work($m));
 *     $f = begin foo($a, $b);       ->  $f = \phasync::go(foo(...), $a, $b);
 *     $f = begin $o->m(x: $a);      ->  $f = \phasync::go($o->m(...), x: $a);
 *     $r = finish $f;               ->  $r = \phasync::await($f);
 *
 * An extension can't change the grammar, and by the time an AST hook runs,
 * `finish $f` is already a syntax error. So zend_compile_file and
 * zend_compile_string are hooked beneath opcache (it wraps them later, at post
 * startup): the source is cut into tokens with PHP's own lexer, and rewritten
 * before the real compiler sees it. Cached opcodes are the rewritten ones, and
 * the setting is in opcache's system id, so no cache serves code compiled with
 * the other setting.
 *
 * The words are contextual: one is a keyword only where PHP gives a syntax error
 * today, so turning them on changes no working code.
 *   - run and begin take a closure literal (function, fn, static function/fn) or a
 *     call. In a call, the last call of the chain runs in the fiber: its callee
 *     becomes a first-class callable, evaluated at the keyword, and its arguments
 *     follow it. Not: a nullsafe chain, new, or `(` directly after the keyword
 *     (a call of a function with that name).
 *   - finish binds like clone: a variable or name and its ->, ::, [] and () chain,
 *     or a run/begin.
 * Nothing is inserted across lines, so line numbers stay. */

#include "Zend/zend_language_scanner.h"
#include "Zend/zend_language_scanner_defs.h"
#include "Zend/zend_language_parser.h"
#include "Zend/zend_system_id.h"
#include "Zend/zend_smart_str.h"

typedef struct {
	int      type;
	uint32_t off, len;
} phasync_kw_tok;

typedef struct {
	const char     *src;
	phasync_kw_tok *t;
	int             n;
	const char    **repl;   /* replacement text of token i, NULL: as is */
	uint8_t        *close;  /* ')' to add after token i */
	bool           *done;   /* a keyword already rewritten */
} phasync_kw;

static zend_op_array *(*phasync_kw_orig_compile_file)(zend_file_handle *fh, int type);
static zend_op_array *(*phasync_kw_orig_compile_string)(zend_string *source, const char *filename, zend_compile_position position);

static bool phasync_kw_skip(int type)
{
	return type == T_WHITESPACE || type == T_COMMENT || type == T_DOC_COMMENT;
}

static int phasync_kw_next(const phasync_kw *k, int i)
{
	for (i++; i < k->n && phasync_kw_skip(k->t[i].type); i++);
	return i;
}

static int phasync_kw_prev(const phasync_kw *k, int i)
{
	for (i--; i >= 0 && phasync_kw_skip(k->t[i].type); i--);
	return i;
}

static bool phasync_kw_is(const phasync_kw *k, int i, const char *word)
{
	size_t len = strlen(word);
	return i >= 0 && i < k->n && k->t[i].type == T_STRING && k->t[i].len == len
	    && memcmp(k->src + k->t[i].off, word, len) == 0;
}

static bool phasync_kw_opens(int type)
{
	return type == '(' || type == '[' || type == '{' || type == T_CURLY_OPEN
	    || type == T_DOLLAR_OPEN_CURLY_BRACES || type == T_ATTRIBUTE;
}

static bool phasync_kw_closes(int type)
{
	return type == ')' || type == ']' || type == '}';
}

/* The token closing the bracket opened at i, or -1. */
static int phasync_kw_match(const phasync_kw *k, int i)
{
	int depth = 0;

	for (; i < k->n; i++) {
		if (phasync_kw_opens(k->t[i].type)) {
			depth++;
		} else if (phasync_kw_closes(k->t[i].type) && --depth == 0) {
			return i;
		}
	}
	return -1;
}

/* The last token of the expression starting at i, where an arrow function's body
 * would end: at ; , ) ] } => as ?> on its level, or a : no ? of its own claims. */
static int phasync_kw_expr_end(const phasync_kw *k, int i)
{
	int last = -1, depth = 0, ternary = 0, arrows = 0;

	for (; i < k->n; i = phasync_kw_next(k, i)) {
		int type = k->t[i].type;

		if (phasync_kw_opens(type)) {
			depth++;
		} else if (phasync_kw_closes(type)) {
			if (depth == 0) {
				break;
			}
			depth--;
		} else if (depth == 0) {
			if (type == ';' || type == ',' || type == T_AS || type == T_CLOSE_TAG) {
				break;
			}
			if (type == T_FN) {
				arrows++;                  /* a nested arrow function's => is its own */
			} else if (type == T_DOUBLE_ARROW) {
				if (arrows == 0) {
					break;
				}
				arrows--;
			} else if (type == '?') {
				ternary++;
			} else if (type == ':') {
				if (ternary == 0) {
					break;
				}
				ternary--;
			}
		}
		last = i;
	}
	return last;
}

/* The last token of a closure literal starting at i, or -1 if there is none. */
static int phasync_kw_closure(const phasync_kw *k, int i)
{
	int type;

	if (i < k->n && k->t[i].type == T_STATIC) {
		i = phasync_kw_next(k, i);
	}
	if (i >= k->n || (k->t[i].type != T_FUNCTION && k->t[i].type != T_FN)) {
		return -1;
	}
	type = k->t[i].type;
	i = phasync_kw_next(k, i);
	if (i < k->n && (k->t[i].type == '&' || k->t[i].type == T_AMPERSAND_NOT_FOLLOWED_BY_VAR_OR_VARARG
	              || k->t[i].type == T_AMPERSAND_FOLLOWED_BY_VAR_OR_VARARG)) {
		i = phasync_kw_next(k, i);
	}
	if (i >= k->n || k->t[i].type != '(' || (i = phasync_kw_match(k, i)) < 0) {
		return -1;
	}
	if (type == T_FN) {
		for (i = phasync_kw_next(k, i); i < k->n && k->t[i].type != T_DOUBLE_ARROW; i = phasync_kw_next(k, i)) {
			if (k->t[i].type == ';' || k->t[i].type == '{') {
				return -1;                 /* not a return type */
			}
		}
		return i < k->n ? phasync_kw_expr_end(k, phasync_kw_next(k, i)) : -1;
	}
	/* function (...) use (...): type { ... } */
	for (i = phasync_kw_next(k, i); i < k->n && k->t[i].type != '{'; i = phasync_kw_next(k, i)) {
		if (k->t[i].type == '(') {
			i = phasync_kw_match(k, i);
			if (i < 0) {
				return -1;
			}
		} else if (k->t[i].type == ';') {
			return -1;
		}
	}
	return i < k->n ? phasync_kw_match(k, i) : -1;
}

static bool phasync_kw_ident(const phasync_kw *k, int i)
{
	const char *s = k->src + k->t[i].off;
	return k->t[i].len > 0 && (isalpha((unsigned char) s[0]) || s[0] == '_' || (unsigned char) s[0] >= 0x80);
}

/* A variable or name and its ->, ::, [] and () chain starting at i: its last token,
 * or -1. *call is the '(' of the chain's last element if that is a call, else -1. */
static int phasync_kw_chain(const phasync_kw *k, int i, int *call, bool *nullsafe)
{
	int end, type = i < k->n ? k->t[i].type : 0;

	*call = -1;
	*nullsafe = false;
	if (type == T_STATIC) {
		int j = phasync_kw_next(k, i);
		if (j >= k->n || k->t[j].type != T_PAAMAYIM_NEKUDOTAYIM) {
			return -1;
		}
	} else if (type != T_VARIABLE && type != T_STRING && type != T_NAME_QUALIFIED
	        && type != T_NAME_FULLY_QUALIFIED && type != T_NAME_RELATIVE) {
		return -1;
	}
	end = i;
	for (;;) {
		int j = phasync_kw_next(k, end), m;

		if (j >= k->n) {
			break;
		}
		type = k->t[j].type;
		if (type == T_OBJECT_OPERATOR || type == T_NULLSAFE_OBJECT_OPERATOR || type == T_PAAMAYIM_NEKUDOTAYIM) {
			m = phasync_kw_next(k, j);
			if (m >= k->n) {
				return -1;
			}
			if (k->t[m].type == '{') {
				m = phasync_kw_match(k, m);
			} else if (k->t[m].type != T_VARIABLE && !phasync_kw_ident(k, m)) {
				return -1;
			}
			if (m < 0) {
				return -1;
			}
			*nullsafe |= type == T_NULLSAFE_OBJECT_OPERATOR;
			*call = -1;
			end = m;
		} else if (type == '[') {
			if ((m = phasync_kw_match(k, j)) < 0) {
				return -1;
			}
			*call = -1;
			end = m;
		} else if (type == '(') {
			if ((m = phasync_kw_match(k, j)) < 0) {
				return -1;
			}
			*call = j;
			end = m;
		} else {
			break;
		}
	}
	return end;
}

static int phasync_kw_apply(phasync_kw *k, int i);

/* run/begin at i: rewrite it and return the construct's last token, or -1. */
static int phasync_kw_start(phasync_kw *k, int i, const char *fn)
{
	int j = phasync_kw_next(k, i), end, call;
	bool nullsafe;

	if ((end = phasync_kw_closure(k, j)) >= 0) {
		k->repl[i] = fn;
		k->close[end]++;
		return end;
	}
	end = phasync_kw_chain(k, j, &call, &nullsafe);
	if (end < 0 || call < 0 || nullsafe) {
		return -1;
	}
	k->repl[i] = fn;
	/* f(a, b) -> f(...), a, b: the call's ')' then closes ours */
	k->repl[call] = phasync_kw_next(k, call) == end ? "(...)" : "(...), ";
	return end;
}

static int phasync_kw_apply(phasync_kw *k, int i)
{
	int p = phasync_kw_prev(k, i), j, end, call;
	bool nullsafe;

	if (k->done[i]) {
		return -1;
	}
	if (p >= 0) {
		switch (k->t[p].type) {
			case T_OBJECT_OPERATOR: case T_NULLSAFE_OBJECT_OPERATOR: case T_PAAMAYIM_NEKUDOTAYIM:
			case T_FUNCTION: case T_FN: case T_CONST: case T_NEW: case T_CLASS: case T_INTERFACE:
			case T_TRAIT: case T_ENUM: case T_USE: case T_NAMESPACE: case T_GOTO: case T_INSTEADOF:
			case T_AS: case T_EXTENDS: case T_IMPLEMENTS: case T_CASE:
				return -1;
		}
	}
	j = phasync_kw_next(k, i);
	if (j >= k->n || k->t[j].type == '(') {
		return -1;                         /* a call of a function with that name */
	}
	if (phasync_kw_is(k, i, "run")) {
		end = phasync_kw_start(k, i, "\\phasync::run(");
	} else if (phasync_kw_is(k, i, "begin")) {
		end = phasync_kw_start(k, i, "\\phasync::go(");
	} else if (phasync_kw_is(k, i, "finish")) {
		if ((phasync_kw_is(k, j, "begin") || phasync_kw_is(k, j, "run") || phasync_kw_is(k, j, "finish"))
		 && (end = phasync_kw_apply(k, j)) >= 0) {
			/* finish begin f(): the inner one is rewritten already */
		} else if ((end = phasync_kw_chain(k, j, &call, &nullsafe)) < 0) {
			return -1;
		}
		k->repl[i] = "\\phasync::await(";
		k->close[end]++;
	} else {
		return -1;
	}
	if (end >= 0) {
		k->done[i] = true;
	}
	return end;
}

/* Whether any of the words stands alone in src: most files skip the lexer. */
static bool phasync_kw_present(const char *src, size_t len)
{
	static const char *const words[] = { "run", "begin", "finish" };

	for (size_t w = 0; w < 3; w++) {
		size_t wl = strlen(words[w]);
		const char *p = src, *end = src + len;

		while ((p = zend_memnstr(p, words[w], wl, end)) != NULL) {
			unsigned char before = p > src ? (unsigned char) p[-1] : ' ';
			unsigned char after = p + wl < end ? (unsigned char) p[wl] : ' ';
			if (!isalnum(before) && before != '_' && before != '$' && before < 0x80
			 && !isalnum(after) && after != '_' && after < 0x80) {
				return true;
			}
			p += wl;
		}
	}
	return false;
}

/* The rewritten source, or NULL when nothing changes. in_script: the source starts
 * in PHP code (eval) rather than before an open tag. */
static zend_string *phasync_kw_rewrite(const char *src, size_t len, bool in_script)
{
	zend_lex_state saved;
	zval code, tok;
	phasync_kw k = { 0 };
	int cap = 256, type, rewrites = 0, er = EG(error_reporting);
	const char *base;
	zend_string *out = NULL;
	smart_str s = { 0 };
	size_t pos = 0;

	if (CG(multibyte) || !phasync_kw_present(src, len)) {
		return NULL;
	}
	ZVAL_STRINGL(&code, src, len);
	zend_save_lexical_state(&saved);
	EG(error_reporting) = 0;                  /* the compiler reports what the lexer finds */
	zend_prepare_string_for_scanning(&code, ZSTR_EMPTY_ALLOC());
	LANG_SCNG(yy_state) = in_script ? yycST_IN_SCRIPTING : yycINITIAL;
	base = Z_STRVAL(code);
	k.src = base;
	k.t = emalloc(cap * sizeof(*k.t));
	while ((type = lex_scan(&tok, NULL)) != 0 && type != T_ERROR) {
		if (Z_TYPE(tok) != IS_UNDEF) {
			zval_ptr_dtor_nogc(&tok);
			ZVAL_UNDEF(&tok);
		}
		if (k.n == cap) {
			cap *= 2;
			k.t = erealloc(k.t, cap * sizeof(*k.t));
		}
		k.t[k.n].type = type;
		k.t[k.n].off = (uint32_t) ((const char *) LANG_SCNG(yy_text) - base);
		k.t[k.n++].len = (uint32_t) LANG_SCNG(yy_leng);
		if (type == T_HALT_COMPILER) {
			break;                            /* what follows is data */
		}
	}
	zend_restore_lexical_state(&saved);
	EG(error_reporting) = er;

	k.repl = ecalloc(k.n + 1, sizeof(*k.repl));
	k.close = ecalloc(k.n + 1, 1);
	k.done = ecalloc(k.n + 1, sizeof(*k.done));
	for (int i = 0; i < k.n; i++) {
		if (k.t[i].type == T_STRING && (phasync_kw_is(&k, i, "run") || phasync_kw_is(&k, i, "begin")
		                                || phasync_kw_is(&k, i, "finish"))) {
			rewrites += phasync_kw_apply(&k, i) >= 0;
		}
	}
	if (rewrites) {
		for (int i = 0; i < k.n; i++) {
			smart_str_appendl(&s, base + pos, k.t[i].off - pos);
			if (k.repl[i]) {
				smart_str_appends(&s, k.repl[i]);
			} else {
				smart_str_appendl(&s, base + k.t[i].off, k.t[i].len);
			}
			for (int c = 0; c < k.close[i]; c++) {
				smart_str_appendc(&s, ')');
			}
			pos = k.t[i].off + k.t[i].len;
		}
		smart_str_appendl(&s, base + pos, len - pos);
		out = smart_str_extract(&s);
	}
	efree(k.t);
	efree(k.repl);
	efree(k.close);
	efree(k.done);
	zval_ptr_dtor_str(&code);
	return out;
}

static zend_op_array *phasync_kw_compile_file(zend_file_handle *fh, int type)
{
	char *buf;
	size_t len;
	zend_string *out;

	if (zend_stream_fixup(fh, &buf, &len) == SUCCESS && (out = phasync_kw_rewrite(buf, len, false)) != NULL) {
		/* the handle keeps its buffer, which the scanner reads: ours, padded as it expects */
		efree(fh->buf);
		fh->buf = safe_emalloc(1, ZSTR_LEN(out), ZEND_MMAP_AHEAD);
		memcpy(fh->buf, ZSTR_VAL(out), ZSTR_LEN(out));
		memset(fh->buf + ZSTR_LEN(out), 0, ZEND_MMAP_AHEAD);
		fh->len = ZSTR_LEN(out);
		zend_string_release(out);
	}
	return phasync_kw_orig_compile_file(fh, type);
}

static zend_op_array *phasync_kw_compile_string(zend_string *source, const char *filename, zend_compile_position position)
{
	zend_string *out = phasync_kw_rewrite(ZSTR_VAL(source), ZSTR_LEN(source),
	                                      position == ZEND_COMPILE_POSITION_AFTER_OPEN_TAG);
	zend_op_array *op_array;

	if (!out) {
		return phasync_kw_orig_compile_string(source, filename, position);
	}
	op_array = phasync_kw_orig_compile_string(out, filename, position);
	zend_string_release(out);
	return op_array;
}

/* MINIT, with phasync.keywords on: before opcache's post-startup wraps the hooks and
 * computes its system id. */
static void phasync_keywords_minit(void)
{
	zend_add_system_entropy("phasync", "keywords", "1", 1);
	phasync_kw_orig_compile_file = zend_compile_file;
	zend_compile_file = phasync_kw_compile_file;
	phasync_kw_orig_compile_string = zend_compile_string;
	zend_compile_string = phasync_kw_compile_string;
}

static void phasync_keywords_mshutdown(void)
{
	if (phasync_kw_orig_compile_file) {
		zend_compile_file = phasync_kw_orig_compile_file;
		zend_compile_string = phasync_kw_orig_compile_string;
		phasync_kw_orig_compile_file = NULL;
	}
}
