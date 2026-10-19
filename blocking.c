/* Included by phasync.c (single translation unit); not compiled on its own. */

/* Overrides that make blocking calls cooperative: sleep()/usleep()/time_nanosleep()/time_sleep_until(), proc_open() and the process family, DNS, and Memcached -- plus the override install/call machinery they share. */

#ifdef HAVE_FULL_DNS_FUNCS
/* ---- DNS: dns_get_record() / dns_get_mx() / dns_check_record() ------------
 *
 * Portions of this section are derived from PHP's ext/standard/dns.c:
 *   Copyright (c) The PHP Group and Contributors. SPDX-License-Identifier:
 *   BSD-3-Clause. See LICENSE.php-src for the full license text.
 * The parser and the functions' logic are kept as in PHP (identical from 8.2 to
 * master); only the resolver query itself runs on the worker pool. */

#ifndef DNS_T_A
#define DNS_T_A		1
#endif
#ifndef DNS_T_NS
#define DNS_T_NS	2
#endif
#ifndef DNS_T_CNAME
#define DNS_T_CNAME	5
#endif
#ifndef DNS_T_SOA
#define DNS_T_SOA	6
#endif
#ifndef DNS_T_PTR
#define DNS_T_PTR	12
#endif
#ifndef DNS_T_HINFO
#define DNS_T_HINFO	13
#endif
#ifndef DNS_T_MX
#define DNS_T_MX	15
#endif
#ifndef DNS_T_TXT
#define DNS_T_TXT	16
#endif
#ifndef DNS_T_AAAA
#define DNS_T_AAAA	28
#endif
#ifndef DNS_T_SRV
#define DNS_T_SRV	33
#endif
#ifndef DNS_T_NAPTR
#define DNS_T_NAPTR	35
#endif
#ifndef DNS_T_A6
#define DNS_T_A6	38
#endif
#ifndef DNS_T_CAA
#define DNS_T_CAA	257
#endif
#ifndef DNS_T_ANY
#define DNS_T_ANY	255
#endif
#ifndef HFIXEDSZ
#define HFIXEDSZ	12
#endif
#ifndef QFIXEDSZ
#define QFIXEDSZ	4
#endif
#define PHASYNC_DNS_MAXHOSTNAMELEN 1024

typedef union {
	HEADER qb1;
	uint8_t qb2[65536];
} phasync_querybuf;

/* php_dns_free_handle() (php_dns.h) calls this dns.c helper under res_nsearch. */
#if defined(__GLIBC__)
#define php_dns_free_res(__res__) phasync_dns_free_res(__res__)
static void phasync_dns_free_res(struct __res_state *res)
{
	int ns;
	for (ns = 0; ns < MAXNS; ns++) {
		if (res->_u._ext.nsaddrs[ns] != NULL) {
			free(res->_u._ext.nsaddrs[ns]);
			res->_u._ext.nsaddrs[ns] = NULL;
		}
	}
}
#else
#define php_dns_free_res(__res__)
#endif

/* Runs on a worker thread: pure libc, no engine. */
static void phasync_dns_query_worker(phasync_task *t)
{
#if defined(HAVE_DNS_SEARCH)                /* macOS, as PHP's dns.c */
	struct sockaddr_storage from;
	uint32_t fromsize = sizeof(from);
	dns_handle_t handle = dns_open(NULL);
	if (handle == NULL) {
		t->hostok = 0;
		t->result = -1;
		return;
	}
#elif defined(HAVE_RES_NSEARCH)
	struct __res_state state;
	struct __res_state *handle = &state;
	memset(&state, 0, sizeof(state));
	if (res_ninit(handle)) {
		t->hostok = 0;
		t->result = -1;
		return;
	}
#else
	void *handle = NULL;
	res_init();
#endif
	t->hostok = 1;
	t->result = php_dns_search(handle, t->host, C_IN, t->qtype, (u_char *) t->buf, (int) t->count);
	t->err = php_dns_errno(handle);
	php_dns_free_handle(handle);
	(void) handle;
}

static void phasync_pool_run(phasync_task *t);

/* The query on the pool (inline if there's no fiber to yield from). */
static int phasync_dns_search(const char *host, int type, phasync_querybuf *answer, int *dns_errno, int *init_ok)
{
	phasync_task t;
	memset(&t, 0, sizeof(t));
	t.type  = PHASYNC_OP_DNSQUERY;
	t.host  = host;
	t.qtype = type;
	t.buf   = (char *) answer->qb2;
	t.count = sizeof(*answer);
	phasync_pool_run(&t);
	*dns_errno = t.err;
	*init_ok = t.hostok;
	return (int) t.result;
}

#define CHECKCP(n) do { \
	if (cp + n > end) { \
		return NULL; \
	} \
} while (0)

static uint8_t *phasync_dns_parserr(uint8_t *cp, uint8_t *end, phasync_querybuf *answer, int type_to_fetch, int store, bool raw, zval *subarray)
{
	u_short type, class, dlen;
	u_long ttl;
	long n, i;
	u_short s;
	uint8_t *tp, *p;
	char name[PHASYNC_DNS_MAXHOSTNAMELEN] = {0};
	int have_v6_break = 0, in_v6_break = 0;

	ZVAL_UNDEF(subarray);

	n = dn_expand(answer->qb2, end, cp, name, sizeof(name) - 2);
	if (n < 0) {
		return NULL;
	}
	cp += n;

	CHECKCP(10);
	GETSHORT(type, cp);
	GETSHORT(class, cp);
	GETLONG(ttl, cp);
	GETSHORT(dlen, cp);
	CHECKCP(dlen);
	if (dlen == 0) {
		return NULL;
	}
	if (type_to_fetch != DNS_T_ANY && type != type_to_fetch) {
		cp += dlen;
		return cp;
	}
	if (!store) {
		cp += dlen;
		return cp;
	}

	array_init(subarray);
	add_assoc_string(subarray, "host", name);
	add_assoc_string(subarray, "class", "IN");
	add_assoc_long(subarray, "ttl", ttl);
	(void) class;

	if (raw) {
		add_assoc_long(subarray, "type", type);
		add_assoc_stringl(subarray, "data", (char*) cp, (uint32_t) dlen);
		cp += dlen;
		return cp;
	}

	switch (type) {
		case DNS_T_A:
			CHECKCP(4);
			add_assoc_string(subarray, "type", "A");
			snprintf(name, sizeof(name), "%d.%d.%d.%d", cp[0], cp[1], cp[2], cp[3]);
			add_assoc_string(subarray, "ip", name);
			cp += dlen;
			break;
		case DNS_T_MX:
			CHECKCP(2);
			add_assoc_string(subarray, "type", "MX");
			GETSHORT(n, cp);
			add_assoc_long(subarray, "pri", n);
			ZEND_FALLTHROUGH;
		case DNS_T_CNAME:
			if (type == DNS_T_CNAME) {
				add_assoc_string(subarray, "type", "CNAME");
			}
			ZEND_FALLTHROUGH;
		case DNS_T_NS:
			if (type == DNS_T_NS) {
				add_assoc_string(subarray, "type", "NS");
			}
			ZEND_FALLTHROUGH;
		case DNS_T_PTR:
			if (type == DNS_T_PTR) {
				add_assoc_string(subarray, "type", "PTR");
			}
			n = dn_expand(answer->qb2, end, cp, name, (sizeof name) - 2);
			if (n < 0) {
				return NULL;
			}
			cp += n;
			add_assoc_string(subarray, "target", name);
			break;
		case DNS_T_HINFO:
			add_assoc_string(subarray, "type", "HINFO");
			CHECKCP(1);
			n = *cp & 0xFF;
			cp++;
			CHECKCP(n);
			add_assoc_stringl(subarray, "cpu", (char*)cp, n);
			cp += n;
			CHECKCP(1);
			n = *cp & 0xFF;
			cp++;
			CHECKCP(n);
			add_assoc_stringl(subarray, "os", (char*)cp, n);
			cp += n;
			break;
		case DNS_T_CAA:
			add_assoc_string(subarray, "type", "CAA");
			CHECKCP(1);
			n = *cp & 0xFF;
			add_assoc_long(subarray, "flags", n);
			cp++;
			CHECKCP(1);
			n = *cp & 0xFF;
			cp++;
			CHECKCP(n);
			add_assoc_stringl(subarray, "tag", (char*)cp, n);
			cp += n;
			if ( (size_t) dlen < ((size_t)n) + 2 ) {
				return NULL;
			}
			n = dlen - n - 2;
			CHECKCP(n);
			add_assoc_stringl(subarray, "value", (char*)cp, n);
			cp += n;
			break;
		case DNS_T_TXT:
			{
				int l1 = 0, l2 = 0;
				zval entries;
				zend_string *tps;

				add_assoc_string(subarray, "type", "TXT");
				tps = zend_string_alloc(dlen, 0);
				array_init(&entries);
				while (l1 < dlen) {
					n = cp[l1];
					if ((l1 + n) >= dlen) {
						n = dlen - (l1 + 1);
					}
					if (n) {
						memcpy(ZSTR_VAL(tps) + l2 , cp + l1 + 1, n);
						add_next_index_stringl(&entries, (char *) cp + l1 + 1, n);
					}
					l1 = l1 + n + 1;
					l2 = l2 + n;
				}
				ZSTR_VAL(tps)[l2] = '\0';
				ZSTR_LEN(tps) = l2;
				cp += dlen;
				add_assoc_str(subarray, "txt", tps);
				add_assoc_zval(subarray, "entries", &entries);
			}
			break;
		case DNS_T_SOA:
			add_assoc_string(subarray, "type", "SOA");
			n = dn_expand(answer->qb2, end, cp, name, (sizeof name) -2);
			if (n < 0) {
				return NULL;
			}
			cp += n;
			add_assoc_string(subarray, "mname", name);
			n = dn_expand(answer->qb2, end, cp, name, (sizeof name) -2);
			if (n < 0) {
				return NULL;
			}
			cp += n;
			add_assoc_string(subarray, "rname", name);
			CHECKCP(5*4);
			GETLONG(n, cp);
			add_assoc_long(subarray, "serial", n);
			GETLONG(n, cp);
			add_assoc_long(subarray, "refresh", n);
			GETLONG(n, cp);
			add_assoc_long(subarray, "retry", n);
			GETLONG(n, cp);
			add_assoc_long(subarray, "expire", n);
			GETLONG(n, cp);
			add_assoc_long(subarray, "minimum-ttl", n);
			break;
		case DNS_T_AAAA:
			tp = (uint8_t*)name;
			CHECKCP(8*2);
			for(i=0; i < 8; i++) {
				GETSHORT(s, cp);
				if (s != 0) {
					if (tp > (uint8_t *)name) {
						in_v6_break = 0;
						tp[0] = ':';
						tp++;
					}
					tp += snprintf((char*)tp, sizeof(name) - (tp - (uint8_t *) name), "%x", s);
				} else {
					if (!have_v6_break) {
						have_v6_break = 1;
						in_v6_break = 1;
						tp[0] = ':';
						tp++;
					} else if (!in_v6_break) {
						tp[0] = ':';
						tp++;
						tp[0] = '0';
						tp++;
					}
				}
			}
			if (have_v6_break && in_v6_break) {
				tp[0] = ':';
				tp++;
			}
			tp[0] = '\0';
			add_assoc_string(subarray, "type", "AAAA");
			add_assoc_string(subarray, "ipv6", name);
			break;
		case DNS_T_A6:
			p = cp;
			add_assoc_string(subarray, "type", "A6");
			CHECKCP(1);
			n = ((int)cp[0]) & 0xFF;
			cp++;
			add_assoc_long(subarray, "masklen", n);
			tp = (uint8_t*)name;
			if (n > 15) {
				have_v6_break = 1;
				in_v6_break = 1;
				tp[0] = ':';
				tp++;
			}
			if (n % 16 > 8) {
				if (cp[0] != 0) {
					if (tp > (uint8_t *)name) {
						in_v6_break = 0;
						tp[0] = ':';
						tp++;
					}
					snprintf((char*)tp, sizeof(name) - (tp - (uint8_t *) name), "%x", cp[0] & 0xFF);
				} else {
					if (!have_v6_break) {
						have_v6_break = 1;
						in_v6_break = 1;
						tp[0] = ':';
						tp++;
					} else if (!in_v6_break) {
						tp[0] = ':';
						tp++;
						tp[0] = '0';
						tp++;
					}
				}
				cp++;
			}
			for (i = (n + 8) / 16; i < 8; i++) {
				CHECKCP(2);
				GETSHORT(s, cp);
				if (s != 0) {
					if (tp > (uint8_t *)name) {
						in_v6_break = 0;
						tp[0] = ':';
						tp++;
					}
					tp += snprintf((char*)tp, sizeof(name) - (tp - (uint8_t *) name),"%x",s);
				} else {
					if (!have_v6_break) {
						have_v6_break = 1;
						in_v6_break = 1;
						tp[0] = ':';
						tp++;
					} else if (!in_v6_break) {
						tp[0] = ':';
						tp++;
						tp[0] = '0';
						tp++;
					}
				}
			}
			if (have_v6_break && in_v6_break) {
				tp[0] = ':';
				tp++;
			}
			tp[0] = '\0';
			add_assoc_string(subarray, "ipv6", name);
			if (cp < p + dlen) {
				n = dn_expand(answer->qb2, end, cp, name, (sizeof name) - 2);
				if (n < 0) {
					return NULL;
				}
				cp += n;
				add_assoc_string(subarray, "chain", name);
			}
			break;
		case DNS_T_SRV:
			CHECKCP(3*2);
			add_assoc_string(subarray, "type", "SRV");
			GETSHORT(n, cp);
			add_assoc_long(subarray, "pri", n);
			GETSHORT(n, cp);
			add_assoc_long(subarray, "weight", n);
			GETSHORT(n, cp);
			add_assoc_long(subarray, "port", n);
			n = dn_expand(answer->qb2, end, cp, name, (sizeof name) - 2);
			if (n < 0) {
				return NULL;
			}
			cp += n;
			add_assoc_string(subarray, "target", name);
			break;
		case DNS_T_NAPTR:
			CHECKCP(2*2);
			add_assoc_string(subarray, "type", "NAPTR");
			GETSHORT(n, cp);
			add_assoc_long(subarray, "order", n);
			GETSHORT(n, cp);
			add_assoc_long(subarray, "pref", n);
			CHECKCP(1);
			n = (cp[0] & 0xFF);
			cp++;
			CHECKCP(n);
			add_assoc_stringl(subarray, "flags", (char*)cp, n);
			cp += n;
			CHECKCP(1);
			n = (cp[0] & 0xFF);
			cp++;
			CHECKCP(n);
			add_assoc_stringl(subarray, "services", (char*)cp, n);
			cp += n;
			CHECKCP(1);
			n = (cp[0] & 0xFF);
			cp++;
			CHECKCP(n);
			add_assoc_stringl(subarray, "regex", (char*)cp, n);
			cp += n;
			n = dn_expand(answer->qb2, end, cp, name, (sizeof name) - 2);
			if (n < 0) {
				return NULL;
			}
			cp += n;
			add_assoc_string(subarray, "replacement", name);
			break;
		default:
			zval_ptr_dtor(subarray);
			ZVAL_UNDEF(subarray);
			cp += dlen;
			break;
	}
	return cp;
}
#undef CHECKCP
#endif /* HAVE_FULL_DNS_FUNCS */

/* Worker side of PHASYNC_OP_FS: plain syscalls on t->path (absolute). */
static int phasync_fs_exec(phasync_task *t)
{
	struct stat st;
	DIR *d;
	char *p;

	switch (t->fsop) {
		case PHASYNC_FS_WARM:
			lstat(t->path, &st);
			return stat(t->path, &st);
		case PHASYNC_FS_WARM_DIR:
		case PHASYNC_FS_WARM_GLOB:
			if ((d = opendir(t->path)) == NULL) {
				return -1;
			}
			while (readdir(d) != NULL);
			return closedir(d);
		case PHASYNC_FS_UNLINK:
			return unlink(t->path);
		case PHASYNC_FS_RMDIR:
			return rmdir(t->path);
		case PHASYNC_FS_RENAME:
			return rename(t->path, t->path2);
		case PHASYNC_FS_MKDIR_P:
			for (p = t->path; (p = strchr(p + 1, '/')) != NULL; *p = '/') {
				*p = '\0';
				if (mkdir(t->path, t->omode) < 0 && errno != EEXIST) {
					*p = '/';
					return -1;
				}
			}
			ZEND_FALLTHROUGH;
		case PHASYNC_FS_MKDIR:
			return mkdir(t->path, t->omode);
		case PHASYNC_FS_FSYNC:
			return fsync(t->fd);
		case PHASYNC_FS_FDATASYNC:
			return fdatasync(t->fd);
	}
	return -1;
}

static void phasync_task_exec(phasync_task *t)
{
	switch (t->type) {
		case PHASYNC_OP_FS:
			t->result = phasync_fs_exec(t);
			t->err = errno;
			break;
		case PHASYNC_OP_READ:
			do { t->result = read(t->fd, t->buf, t->count); }
			while (t->result < 0 && errno == EINTR);
			t->err = errno;
			break;
		case PHASYNC_OP_WRITE:
			do { t->result = write(t->fd, t->buf, t->count); }
			while (t->result < 0 && errno == EINTR);
			t->err = errno;
			break;
		case PHASYNC_OP_CALL:
			t->call(t->arg);
			break;
		case PHASYNC_OP_OPEN:
			do { t->fd = open(t->path, t->oflags, t->omode); }
			while (t->fd < 0 && errno == EINTR);
			t->err = errno;
			break;
		case PHASYNC_OP_GETHOSTBYNAME: {
#ifdef HAVE_GETHOSTBYNAME_R
			/* php_network_gethostbyname() is gethostbyname_r() on Linux: use the
			 * same call so the addresses (and their order) are exactly native. */
			struct hostent he, *res = NULL;
			char stackbuf[8192], *buf = stackbuf;
			size_t buflen = sizeof(stackbuf);
			int herr = 0, rc;
			while ((rc = gethostbyname_r(t->host, &he, buf, buflen, &res, &herr)) == ERANGE
			       && buflen < (1 << 20)) {
				buflen *= 2;
				buf = (buf == stackbuf) ? malloc(buflen) : realloc(buf, buflen);
				if (!buf) break;
			}
			t->hostok = (rc == 0 && res != NULL);
#else
			/* PHP calls gethostbyname() where there is no _r (macOS: thread-safe there). */
			struct hostent *res = gethostbyname(t->host);
			t->hostok = res != NULL;
#endif
			t->naddrs = 0;
			if (t->hostok && res->h_addrtype == AF_INET) {
				for (int i = 0; res->h_addr_list[i] && t->naddrs < (int) (sizeof(t->addrs) / sizeof(t->addrs[0])); i++) {
					memcpy(&t->addrs[t->naddrs++], res->h_addr_list[i], sizeof(struct in_addr));
				}
			}
#ifdef HAVE_GETHOSTBYNAME_R
			if (buf && buf != stackbuf) free(buf);
#endif
			break;
		}
#ifdef HAVE_FULL_DNS_FUNCS
		case PHASYNC_OP_DNSQUERY:
			/* Exactly the resolver calls native makes (php_dns.h picks them from how
			 * PHP was built): result in t->result, php_dns_errno() in t->err,
			 * t->hostok = 0 if the resolver state could not even be initialised. */
			phasync_dns_query_worker(t);
			break;
#endif
		case PHASYNC_OP_NAMEINFO:
			t->hostok = getnameinfo((struct sockaddr *) &t->sa, t->salen, t->hostresult,
				sizeof(t->hostresult), NULL, 0, NI_NAMEREQD) == 0;
			break;
		case PHASYNC_OP_GETADDRINFO: {
			t->ai = NULL;
			t->result = getaddrinfo(t->host, t->serv[0] ? t->serv : NULL, &t->hints, &t->ai);
			break;
		}
	}
}

/* ---- proc_open() override: wrap the pipe streams it produces -------------- */

static void phasync_wrap_pipes_array(zval *pipes)
{
	zval *elem;
	php_stream *stream;

	ZVAL_DEREF(pipes);
	if (Z_TYPE_P(pipes) != IS_ARRAY) {
		return;
	}
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(pipes), elem) {
		ZVAL_DEREF(elem);
		if (Z_TYPE_P(elem) != IS_RESOURCE) {
			continue;
		}
		stream = (php_stream *) zend_fetch_resource2_ex(elem, NULL,
			php_file_le_stream(), php_file_le_pstream());
		if (stream) {
			phasync_wrap_stream(stream, PHASYNC_MODE_RAW);
		}
	} ZEND_HASH_FOREACH_END();
}

static ZEND_NAMED_FUNCTION(phasync_proc_open_override)
{
	PHASYNC_G(orig_proc_open)(INTERNAL_FUNCTION_PARAM_PASSTHRU);

	if (!EG(exception) && ZEND_NUM_ARGS() >= 3) {
		/* arg #3 ($pipes) is by-ref and now holds the pipe stream resources */
		zval *pipes = ZEND_CALL_ARG(execute_data, 3);
		if (pipes) {
			phasync_wrap_pipes_array(pipes);
		}
	}
}

/* ---- sleep()/usleep() override ------------------------------------------- */

/* Seconds -> microseconds for the sleep handler, clamped: native PHP accepts
 * absurdly long sleeps, and the conversion must not overflow zend_long. */
static zend_long phasync_sec_to_usec(double sec)
{
	double us = sec * 1000000.0;
	return us >= (double) ZEND_LONG_MAX ? ZEND_LONG_MAX : (zend_long) us;
}

static ZEND_NAMED_FUNCTION(phasync_sleep_override)
{
	zend_long seconds;

	/* Only intercept when a fiber is actually running. The scheduler itself
	 * idle-waits by calling these same sleep functions from the main (non-fiber)
	 * context; delegating those to the handler (which can only Fiber::suspend
	 * inside a fiber) would collapse the wait to a no-op and spin the loop. */
	if (phasync_sleep_handler() == NULL || EG(active_fiber) == NULL || phasync_from_c()) {
		PHASYNC_G(orig_sleep)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(seconds)
	ZEND_PARSE_PARAMETERS_END();

	if (seconds < 0) {
		zend_argument_value_error(1, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	phasync_call_sleep(phasync_sleep_handler(), phasync_sec_to_usec((double) seconds));
	RETURN_LONG(0);
}

/* Remembers the registered header callback: sapi_send_headers() runs it with the
 * headers unsent and no marker left (poller.c). */
static ZEND_NAMED_FUNCTION(phasync_header_register_callback_override)
{
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;

	PHASYNC_G(orig_header_register_callback)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	if (!EG(exception) && !SG(headers_sent)
	 && zend_parse_parameters(ZEND_NUM_ARGS(), "f", &fci, &fcc) == SUCCESS) {
		PHASYNC_G(header_cb) = fcc.function_handler;
	}
}

static ZEND_NAMED_FUNCTION(phasync_usleep_override)
{
	zend_long usec;

	if (phasync_sleep_handler() == NULL || EG(active_fiber) == NULL || phasync_from_c()) {
		PHASYNC_G(orig_usleep)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(usec)
	ZEND_PARSE_PARAMETERS_END();

	if (usec < 0) {
		zend_argument_value_error(1, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	phasync_call_sleep(phasync_sleep_handler(), usec);
}

static ZEND_NAMED_FUNCTION(phasync_time_nanosleep_override)
{
	zend_long sec, nsec;

	if (phasync_sleep_handler() == NULL || EG(active_fiber) == NULL || phasync_from_c()) {
		PHASYNC_G(orig_time_nanosleep)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_LONG(sec)
		Z_PARAM_LONG(nsec)
	ZEND_PARSE_PARAMETERS_END();

	if (sec < 0) {
		zend_argument_value_error(1, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	if (nsec < 0) {
		zend_argument_value_error(2, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	if (nsec > 999999999) {             /* native nanosleep() fails with EINVAL */
		zend_value_error("Nanoseconds was not in the range 0 to 999 999 999 or seconds was negative");
		RETURN_THROWS();
	}
	phasync_call_sleep(phasync_sleep_handler(),
		phasync_sec_to_usec((double) sec + (double) nsec / 1000000000.0));
	RETURN_TRUE;
}

static ZEND_NAMED_FUNCTION(phasync_time_sleep_until_override)
{
	double ts, now;
	struct timeval tv;
	zend_long usec;

	if (phasync_sleep_handler() == NULL || EG(active_fiber) == NULL || phasync_from_c()) {
		PHASYNC_G(orig_time_sleep_until)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_DOUBLE(ts)
	ZEND_PARSE_PARAMETERS_END();

	gettimeofday(&tv, NULL);
	now = (double) tv.tv_sec + (double) tv.tv_usec / 1000000.0;
	usec = (ts > now) ? phasync_sec_to_usec(ts - now) : 0;
	phasync_call_sleep(phasync_sleep_handler(), usec);
	RETURN_TRUE;
}

static ZEND_NAMED_FUNCTION(phasync_gethostbyname_override)
{
	char *host;
	size_t hostlen;
	phasync_task t;
	char ip[INET_ADDRSTRLEN];

	if (!phasync_reading() || EG(active_fiber) == NULL) {
		PHASYNC_G(orig_gethostbyname)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_PATH(host, hostlen)
	ZEND_PARSE_PARAMETERS_END();

	if (hostlen == 0 || hostlen > MAXFQDNLEN) {
		PHASYNC_G(orig_gethostbyname)(INTERNAL_FUNCTION_PARAM_PASSTHRU);   /* native warning */
		return;
	}
	memset(&t, 0, sizeof(t));
	t.type = PHASYNC_OP_GETHOSTBYNAME;
	t.host = host;
	phasync_pool_run(&t);   /* resolves on a pool thread; parks the fiber */

	if (t.hostok && t.naddrs > 0 && inet_ntop(AF_INET, &t.addrs[0], ip, sizeof(ip))) {
		RETURN_STRING(ip);
	}
	RETURN_STRINGL(host, hostlen);   /* native returns the input unchanged on failure */
}

/* gethostbynamel(): the same lookup as gethostbyname(), every address. */
static ZEND_NAMED_FUNCTION(phasync_gethostbynamel_override)
{
	char *host;
	size_t hostlen;
	phasync_task t;
	char ip[INET_ADDRSTRLEN];

	if (!phasync_reading() || EG(active_fiber) == NULL) {
		PHASYNC_G(orig_gethostbynamel)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_PATH(host, hostlen)
	ZEND_PARSE_PARAMETERS_END();

	if (hostlen > MAXFQDNLEN) {
		PHASYNC_G(orig_gethostbynamel)(INTERNAL_FUNCTION_PARAM_PASSTHRU);  /* native warning */
		return;
	}
	memset(&t, 0, sizeof(t));
	t.type = PHASYNC_OP_GETHOSTBYNAME;
	t.host = host;
	phasync_pool_run(&t);
	if (EG(exception)) {
		return;
	}
	if (!t.hostok) {
		RETURN_FALSE;
	}
	array_init(return_value);
	for (int i = 0; i < t.naddrs; i++) {
		if (inet_ntop(AF_INET, &t.addrs[i], ip, sizeof(ip))) {
			add_next_index_string(return_value, ip);
		}
	}
}

#ifdef HAVE_FULL_DNS_FUNCS
/* dns_check_record() / checkdnsrr() — logic as PHP's (see the DNS section's
 * notice); the query runs on the pool. Argument errors go to the original so
 * their (version-specific) messages stay exact. */
static ZEND_NAMED_FUNCTION(phasync_dns_check_record_override)
{
	phasync_querybuf *answer;
	char *hostname;
	size_t hostname_len;
	zend_string *rectype = NULL;
	int type = DNS_T_MX, i, dns_errno, init_ok;

	if (!phasync_reading() || EG(active_fiber) == NULL) {
		PHASYNC_G(orig_dns_check_record)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_PATH(hostname, hostname_len)
		Z_PARAM_OPTIONAL
		Z_PARAM_STR(rectype)
	ZEND_PARSE_PARAMETERS_END();

	if (rectype) {
		if (zend_string_equals_literal_ci(rectype, "A")) type = DNS_T_A;
		else if (zend_string_equals_literal_ci(rectype, "NS")) type = DNS_T_NS;
		else if (zend_string_equals_literal_ci(rectype, "MX")) type = DNS_T_MX;
		else if (zend_string_equals_literal_ci(rectype, "PTR")) type = DNS_T_PTR;
		else if (zend_string_equals_literal_ci(rectype, "ANY")) type = DNS_T_ANY;
		else if (zend_string_equals_literal_ci(rectype, "SOA")) type = DNS_T_SOA;
		else if (zend_string_equals_literal_ci(rectype, "CAA")) type = DNS_T_CAA;
		else if (zend_string_equals_literal_ci(rectype, "TXT")) type = DNS_T_TXT;
		else if (zend_string_equals_literal_ci(rectype, "CNAME")) type = DNS_T_CNAME;
		else if (zend_string_equals_literal_ci(rectype, "AAAA")) type = DNS_T_AAAA;
		else if (zend_string_equals_literal_ci(rectype, "SRV")) type = DNS_T_SRV;
		else if (zend_string_equals_literal_ci(rectype, "NAPTR")) type = DNS_T_NAPTR;
		else if (zend_string_equals_literal_ci(rectype, "A6")) type = DNS_T_A6;
		else type = -1;
	}
	if (hostname_len == 0 || type == -1) {
		PHASYNC_G(orig_dns_check_record)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}

	answer = ecalloc(1, sizeof(*answer));
	i = phasync_dns_search(hostname, type, answer, &dns_errno, &init_ok);
	if (EG(exception) || !init_ok || i < 0) {
		efree(answer);
		if (!EG(exception)) {
			RETVAL_FALSE;
		}
		return;
	}
	RETVAL_BOOL(ntohs(((HEADER *) answer)->ancount) != 0);
	efree(answer);
}

/* dns_get_record() — logic as PHP's; each per-type query runs on the pool. */
static ZEND_NAMED_FUNCTION(phasync_dns_get_record_override)
{
	char *hostname;
	size_t hostname_len;
	zend_long type_param = PHP_DNS_ANY;
	zval *authns = NULL, *addtl = NULL;
	int type_to_fetch;
	int dns_errno, init_ok;
	HEADER *hp;
	phasync_querybuf *answer;
	uint8_t *cp = NULL, *end = NULL;
	int n, qd, an, ns = 0, ar = 0;
	int type, first_query = 1, store_results = 1;
	bool raw = 0;

	if (!phasync_reading() || EG(active_fiber) == NULL) {
		PHASYNC_G(orig_dns_get_record)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	ZEND_PARSE_PARAMETERS_START(1, 5)
		Z_PARAM_PATH(hostname, hostname_len)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(type_param)
		Z_PARAM_ZVAL(authns)
		Z_PARAM_ZVAL(addtl)
		Z_PARAM_BOOL(raw)
	ZEND_PARSE_PARAMETERS_END();

	/* Invalid types are rejected by the original, with its own message. */
	if ((!raw && (type_param & ~PHP_DNS_ALL) && type_param != PHP_DNS_ANY)
	 || (raw && (type_param < 1 || type_param > 0xFFFF))) {
		PHASYNC_G(orig_dns_get_record)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	if (authns) {
		authns = zend_try_array_init(authns);
		if (!authns) {
			RETURN_THROWS();
		}
	}
	if (addtl) {
		addtl = zend_try_array_init(addtl);
		if (!addtl) {
			RETURN_THROWS();
		}
	}

	array_init(return_value);
	answer = emalloc(sizeof(*answer));

	if (raw) {
		type = -1;
	} else if (type_param == PHP_DNS_ANY) {
		type = PHP_DNS_NUM_TYPES + 1;
	} else {
		type = 0;
	}

	for ( ;
		type < (addtl ? (PHP_DNS_NUM_TYPES + 2) : PHP_DNS_NUM_TYPES) || first_query;
		type++
	) {
		first_query = 0;
		switch (type) {
			case -1:
				type_to_fetch = type_param;
				type = PHP_DNS_NUM_TYPES - 1;
				break;
			case 0:  type_to_fetch = type_param&PHP_DNS_A     ? DNS_T_A     : 0; break;
			case 1:  type_to_fetch = type_param&PHP_DNS_NS    ? DNS_T_NS    : 0; break;
			case 2:  type_to_fetch = type_param&PHP_DNS_CNAME ? DNS_T_CNAME : 0; break;
			case 3:  type_to_fetch = type_param&PHP_DNS_SOA   ? DNS_T_SOA   : 0; break;
			case 4:  type_to_fetch = type_param&PHP_DNS_PTR   ? DNS_T_PTR   : 0; break;
			case 5:  type_to_fetch = type_param&PHP_DNS_HINFO ? DNS_T_HINFO : 0; break;
			case 6:  type_to_fetch = type_param&PHP_DNS_MX    ? DNS_T_MX    : 0; break;
			case 7:  type_to_fetch = type_param&PHP_DNS_TXT   ? DNS_T_TXT   : 0; break;
			case 8:  type_to_fetch = type_param&PHP_DNS_AAAA  ? DNS_T_AAAA  : 0; break;
			case 9:  type_to_fetch = type_param&PHP_DNS_SRV   ? DNS_T_SRV   : 0; break;
			case 10: type_to_fetch = type_param&PHP_DNS_NAPTR ? DNS_T_NAPTR : 0; break;
			case 11: type_to_fetch = type_param&PHP_DNS_A6    ? DNS_T_A6    : 0; break;
			case 12: type_to_fetch = type_param&PHP_DNS_CAA   ? DNS_T_CAA   : 0; break;
			case PHP_DNS_NUM_TYPES:
				store_results = 0;
				continue;
			default:
			case (PHP_DNS_NUM_TYPES + 1):
				type_to_fetch = DNS_T_ANY;
				break;
		}

		if (type_to_fetch) {
			memset(answer, 0, sizeof(*answer));
			n = phasync_dns_search(hostname, type_to_fetch, answer, &dns_errno, &init_ok);
			if (EG(exception)) {
				efree(answer);
				return;
			}
			if (!init_ok) {
				efree(answer);
				zend_array_destroy(Z_ARR_P(return_value));
				RETURN_FALSE;
			}
			if (n < 0) {
				switch (dns_errno) {
					case NO_DATA:
					case HOST_NOT_FOUND:
						continue;
					case NO_RECOVERY:
						php_error_docref(NULL, E_WARNING, "An unexpected server failure occurred.");
						break;
					case TRY_AGAIN:
						php_error_docref(NULL, E_WARNING, "A temporary server error occurred.");
						break;
					default:
						php_error_docref(NULL, E_WARNING, "DNS Query failed");
				}
				efree(answer);
				zend_array_destroy(Z_ARR_P(return_value));
				RETURN_FALSE;
			}

			cp = answer->qb2 + HFIXEDSZ;
			end = answer->qb2 + n;
			hp = (HEADER *) answer;
			qd = ntohs(hp->qdcount);
			an = ntohs(hp->ancount);
			ns = ntohs(hp->nscount);
			ar = ntohs(hp->arcount);

			while (qd-- > 0) {
				n = dn_skipname(cp, end);
				if (n < 0) {
					php_error_docref(NULL, E_WARNING, "Unable to parse DNS data received");
					efree(answer);
					zend_array_destroy(Z_ARR_P(return_value));
					RETURN_FALSE;
				}
				cp += n + QFIXEDSZ;
			}

			while (an-- && cp && cp < end) {
				zval retval;
				cp = phasync_dns_parserr(cp, end, answer, type_to_fetch, store_results, raw, &retval);
				if (Z_TYPE(retval) != IS_UNDEF && store_results) {
					add_next_index_zval(return_value, &retval);
				}
			}

			if (authns || addtl) {
				while (ns-- > 0 && cp && cp < end) {
					zval retval;
					cp = phasync_dns_parserr(cp, end, answer, DNS_T_ANY, authns != NULL, raw, &retval);
					if (Z_TYPE(retval) != IS_UNDEF) {
						add_next_index_zval(authns, &retval);
					}
				}
			}

			if (addtl) {
				while (ar-- > 0 && cp && cp < end) {
					zval retval;
					cp = phasync_dns_parserr(cp, end, answer, DNS_T_ANY, 1, raw, &retval);
					if (Z_TYPE(retval) != IS_UNDEF) {
						add_next_index_zval(addtl, &retval);
					}
				}
			}
		}
	}
	efree(answer);
}

/* dns_get_mx() / getmxrr() — logic as PHP's; the query runs on the pool. */
static ZEND_NAMED_FUNCTION(phasync_dns_get_mx_override)
{
	char *hostname;
	size_t hostname_len;
	zval *mx_list, *weight_list = NULL;
	int count, qdc, dns_errno, init_ok;
	u_short type, weight;
	phasync_querybuf *answer;
	char buf[PHASYNC_DNS_MAXHOSTNAMELEN] = {0};
	HEADER *hp;
	uint8_t *cp, *end;
	int i;

	if (!phasync_reading() || EG(active_fiber) == NULL) {
		PHASYNC_G(orig_dns_get_mx)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	ZEND_PARSE_PARAMETERS_START(2, 3)
		Z_PARAM_PATH(hostname, hostname_len)
		Z_PARAM_ZVAL(mx_list)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(weight_list)
	ZEND_PARSE_PARAMETERS_END();

	mx_list = zend_try_array_init(mx_list);
	if (!mx_list) {
		RETURN_THROWS();
	}
	if (weight_list) {
		weight_list = zend_try_array_init(weight_list);
		if (!weight_list) {
			RETURN_THROWS();
		}
	}

	answer = ecalloc(1, sizeof(*answer));
	i = phasync_dns_search(hostname, DNS_T_MX, answer, &dns_errno, &init_ok);
	if (EG(exception) || !init_ok || i < 0) {
		efree(answer);
		if (!EG(exception)) {
			RETVAL_FALSE;
		}
		return;
	}
	hp = (HEADER *) answer;
	cp = answer->qb2 + HFIXEDSZ;
	end = answer->qb2 + i;
	for (qdc = ntohs((unsigned short)hp->qdcount); qdc--; cp += i + QFIXEDSZ) {
		if ((i = dn_skipname(cp, end)) < 0 ) {
			efree(answer);
			RETURN_FALSE;
		}
	}
	count = ntohs((unsigned short)hp->ancount);
	while (--count >= 0 && cp < end) {
		if ((i = dn_skipname(cp, end)) < 0 ) {
			efree(answer);
			RETURN_FALSE;
		}
		cp += i;
		GETSHORT(type, cp);
		cp += INT16SZ + INT32SZ;
		GETSHORT(i, cp);
		if (type != DNS_T_MX) {
			cp += i;
			continue;
		}
		GETSHORT(weight, cp);
		if ((i = dn_expand(answer->qb2, end, cp, buf, sizeof(buf)-1)) < 0) {
			efree(answer);
			RETURN_FALSE;
		}
		cp += i;
		add_next_index_string(mx_list, buf);
		if (weight_list) {
			add_next_index_long(weight_list, weight);
		}
	}
	efree(answer);
	RETURN_BOOL(zend_hash_num_elements(Z_ARRVAL_P(mx_list)) != 0);
}
#endif /* HAVE_FULL_DNS_FUNCS */

/* gethostbyaddr(): the reverse lookup (getnameinfo, NI_NAMEREQD) on the pool. */
static ZEND_NAMED_FUNCTION(phasync_gethostbyaddr_override)
{
	char *addr;
	size_t addrlen;
	phasync_task t;

	if (!phasync_reading() || EG(active_fiber) == NULL) {
		PHASYNC_G(orig_gethostbyaddr)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_PATH(addr, addrlen)
	ZEND_PARSE_PARAMETERS_END();

	memset(&t, 0, sizeof(t));
	{
		struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *) &t.sa;
		struct sockaddr_in  *sa4 = (struct sockaddr_in *) &t.sa;
		if (inet_pton(AF_INET6, addr, &sa6->sin6_addr) == 1) {   /* same order as native */
			sa6->sin6_family = AF_INET6;
			t.salen = sizeof(*sa6);
		} else if (inet_pton(AF_INET, addr, &sa4->sin_addr) == 1) {
			sa4->sin_family = AF_INET;
			t.salen = sizeof(*sa4);
		} else {
			PHASYNC_G(orig_gethostbyaddr)(INTERNAL_FUNCTION_PARAM_PASSTHRU);  /* native warning */
			return;
		}
	}
	t.type = PHASYNC_OP_NAMEINFO;
	phasync_pool_run(&t);
	if (EG(exception)) {
		return;
	}
	if (t.hostok) {
		RETURN_STRING(t.hostresult);
	}
	RETURN_STRINGL(addr, addrlen);   /* native: no name -> the address itself */
}

/* Translate an fopen() mode string to open(2) flags (for the FIFO fast path). */
static int phasync_mode_to_oflags(const char *mode)
{
	int flags;
	bool plus = strchr(mode, '+') != NULL;

	switch (mode[0]) {
		case 'r': flags = plus ? O_RDWR : O_RDONLY; break;
		case 'w': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
		case 'a': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
		case 'x': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_EXCL; break;
		case 'c': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT; break;
		default:  flags = O_RDONLY; break;
	}
	return flags;
}

/* fopen() override.
 *   - Named pipe (FIFO): open() itself blocks in the kernel rendezvous until a
 *     peer opens the other end — before any fd exists, so there is nothing for a
 *     cooperative scheduler to poll and single-threaded code deadlocks. We run
 *     the open() on a dedicated thread so the fiber can yield until the peer
 *     shows up, then RAW-wrap the fd (after open, a FIFO honors O_NONBLOCK).
 *   - Regular file: open normally, then POOL-wrap so fread/fwrite offload to the
 *     worker pool (regular files are never readiness-pollable).
 *   - Everything else (wrappers, char/block devices, dirs): untouched. */
static ZEND_NAMED_FUNCTION(phasync_fopen_override)
{
	zend_string *filename, *mode;
	bool use_include_path = 0;
	zval *zcontext = NULL;
	php_stream *stream;
	struct stat st;

	if (!phasync_reading()) {
		PHASYNC_G(orig_fopen)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}

	ZEND_PARSE_PARAMETERS_START(2, 4)
		Z_PARAM_STR(filename)
		Z_PARAM_STR(mode)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL(use_include_path)
		Z_PARAM_RESOURCE_OR_NULL(zcontext)
	ZEND_PARSE_PARAMETERS_END();

	/* php://stdin|stdout|stderr and php://fd/N are backed by real OS descriptors,
	 * so wrap them RAW to make them cooperative too. Other php:// streams
	 * (memory/temp/input/output) and every other wrapper are not fd-backed and
	 * fall through untouched below. */
	if (zcontext == NULL && !use_include_path
	 && (strncasecmp(ZSTR_VAL(filename), "php://std", sizeof("php://std") - 1) == 0
	  || strncasecmp(ZSTR_VAL(filename), "php://fd/", sizeof("php://fd/") - 1) == 0)) {
		PHASYNC_G(orig_fopen)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		if (Z_TYPE_P(return_value) == IS_RESOURCE) {
			php_stream *s = NULL;
			php_stream_from_zval_no_verify(s, return_value);
			if (s && phasync_stream_fd(s) != -1) {
				phasync_wrap_stream(s, PHASYNC_MODE_RAW);
			}
		}
		return;
	}

	/* Only plain local paths get special handling; wrappers (http://, php://,
	 * data:// …) and include-path/context lookups fall straight through. */
	if (zcontext != NULL || use_include_path
	 || ZSTR_LEN(filename) == 0 || ZSTR_LEN(filename) >= PATH_MAX
	 || strstr(ZSTR_VAL(filename), "://") != NULL) {
		PHASYNC_G(orig_fopen)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}

	/* On a network/FUSE mount even the stat() below can stall: warm it on the pool. */
	if (phasync_reading() && EG(active_fiber)) {
		phasync_task w;
		zval z;
		ZVAL_STR(&z, filename);
		if (phasync_fs_path(&z, w.path) == 1) {
			w.type = PHASYNC_OP_FS;
			w.fsop = PHASYNC_FS_WARM;
			phasync_pool_run(&w);
			if (EG(exception)) {
				RETURN_THROWS();
			}
		}
	}

	/* FIFO? Detect before opening — the open() is what blocks. */
	if (stat(ZSTR_VAL(filename), &st) == 0 && S_ISFIFO(st.st_mode)) {
		phasync_task t;
		memset(&t, 0, sizeof(t));
		t.type   = PHASYNC_OP_OPEN;
		t.fd     = -1;   /* so a cancelled (never-assigned) open never closes fd 0 */
		t.oflags = phasync_mode_to_oflags(ZSTR_VAL(mode));
		t.omode  = 0666;
		memcpy(t.path, ZSTR_VAL(filename), ZSTR_LEN(filename) + 1);

		phasync_pool_run_dedicated(&t);   /* blocks on its own thread, fiber yields */

		if (t.fd < 0) {
			php_error_docref(NULL, E_WARNING, "fopen(%s): %s",
				ZSTR_VAL(filename), strerror(t.err));
			RETURN_FALSE;
		}
		stream = php_stream_fopen_from_fd(t.fd, ZSTR_VAL(mode), NULL);
		if (!stream) {
			close(t.fd);
			RETURN_FALSE;
		}
		phasync_wrap_stream(stream, PHASYNC_MODE_RAW);
		php_stream_to_zval(stream, return_value);
		return;
	}

	/* Not a FIFO: open normally, then POOL-wrap regular files that are offloaded. */
	PHASYNC_G(orig_fopen)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	if (Z_TYPE_P(return_value) == IS_RESOURCE) {
		php_stream *s = NULL;
		php_stream_from_zval_no_verify(s, return_value);
		if (s) {
			php_socket_t fd = phasync_stream_fd(s);
			if (fd != -1 && phasync_fs_fd_offloaded(fd)) {
				phasync_wrap_stream(s, PHASYNC_MODE_POOL);
			}
		}
	}
}

/* ---- process family: popen(), exec(), system(), passthru(), shell_exec(),
 * proc_close() --------------------------------------------------------------
 *
 * Their pipe reads/writes cooperate through the wrapped ops like any pipe; the
 * blocking wait for the child in pclose()/proc_close() cooperates through a pidfd
 * of the child. popen() doesn't expose the pid, so it is found by diffing this
 * thread's /proc children around the spawn (an unreaped child's pid can't be
 * reused, so that is race-free). Without /proc children or pidfd_open() (Linux
 * < 5.3) the child wait stays native. */

/* This thread's children (Linux: from /proc; macOS: the process's); -1 when
 * unavailable. */
static int phasync_children(pid_t *out)
{
#ifdef __APPLE__
	int bytes = proc_listpids(PROC_PPID_ONLY, (uint32_t) getpid(), out, PHASYNC_MAX_CHILDREN * sizeof(pid_t));

	if (bytes < 0 || bytes >= (int) (PHASYNC_MAX_CHILDREN * sizeof(pid_t))) {
		return -1;                       /* unreadable, or too many to be sure */
	}
	return bytes / (int) sizeof(pid_t);
#else
	char path[64], buf[PHASYNC_MAX_CHILDREN * 8], *p, *end;
	ssize_t len;
	int fd, n = 0;

	snprintf(path, sizeof(path), "/proc/self/task/%ld/children", (long) syscall(SYS_gettid));
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) {
		return -1;
	}
	len = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (len < 0 || len == (ssize_t) sizeof(buf) - 1) {
		return -1;                       /* unreadable, or too many to be sure */
	}
	buf[len] = '\0';
	for (p = buf; n < PHASYNC_MAX_CHILDREN; p = end) {
		long pid = strtol(p, &end, 10);
		if (end == p) {
			break;
		}
		out[n++] = (pid_t) pid;
	}
	return n;
#endif
}

/* A descriptor that turns readable once process pid exits (a pidfd; a kqueue
 * watching NOTE_EXIT elsewhere); -1 if there is none. */
static int phasync_proc_fd(pid_t pid)
{
#ifdef __linux__
	return (int) syscall(SYS_pidfd_open, pid, 0);
#else
	struct kevent ev;
	int kq = phasync_mux_open();

	if (kq >= 0) {
		EV_SET(&ev, pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, NULL);
		if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
			close(kq);
			kq = -1;
		}
	}
	return kq;
#endif
}

/* A pidfd of the one child spawned since sp was taken; -1 if not exactly one. */
static int phasync_new_child_pidfd(const phasync_spawn *sp)
{
	pid_t now[PHASYNC_MAX_CHILDREN], pid = 0;
	int n, i, j;

	if (sp->n < 0 || (n = phasync_children(now)) < 0) {
		return -1;
	}
	for (i = 0; i < n; i++) {
		for (j = 0; j < sp->n && sp->before[j] != now[i]; j++);
		if (j < sp->n) {
			continue;
		}
		if (pid) {
			return -1;
		}
		pid = now[i];
	}
	return pid ? phasync_proc_fd(pid) : -1;
}

static ssize_t (*phasync_stdio_read_orig)(php_stream *stream, char *buf, size_t count);
static int (*phasync_stdio_close_orig)(php_stream *stream, int close_handle);

/* Streams whose close op deregisters them (phasync_stream_forget()): stdio
 * streams and every wrapped stream. */
static bool phasync_has_close_hook(php_stream *stream)
{
	return stream->ops == &php_stream_stdio_ops
		|| stream->ops->read == phasync_wrapped_read
		|| stream->ops->read == phasync_wrapped_read_tls
		|| stream->ops->read == phasync_wrapped_read_pool;
}

static void phasync_inflight_close(php_stream *stream);

static int phasync_stdio_close(php_stream *stream, int close_handle)
{
	phasync_inflight_close(stream);
	return phasync_stdio_close_orig(stream, close_handle);
}
static ssize_t (*phasync_stdio_write_orig)(php_stream *stream, const char *buf, size_t count);
static int (*phasync_stdio_set_option_orig)(php_stream *stream, int option, int value, void *ptrparam);

/* The leading fields of plain_wrapper.c's php_stdio_stream_data, the same from
 * PHP 8.2 to master. */
typedef struct {
	FILE *file;
	int   fd;
} phasync_stdio_head;

/* php_stream_stdio_ops.read/write see every plain file stream: fopen()'d ones on
 * a local disk, and those opened internally (file_get_contents(),
 * file_put_contents(), file(), copy(), readfile(), md5_file() ...), which bypass
 * the fopen() override. Inside a scope, in a fiber, their reads run as
 * phasync_fs_fd_mode() says (and mmap is declined unless inline), and writes go
 * to the pool for a pool file. Returns PHASYNC_FS_INLINE for a stream that stays
 * native, and sets *fdp otherwise.
 * The stream keeps the stdio ops: PHP checks php_stream_is(STDIO) on streams it
 * builds internally (php://temp's spill file, for one) and breaks if they change.
 * A stream with a C FILE* (its own buffer) stays native. Never for a file being
 * compiled: include/require (under a user frame) and the internal functions that
 * compile (opcache_compile_file() ...) read through a stdio stream too, but must
 * not suspend mid-compile. Zend's streams are the unbuffered ones marked
 * auto-cleanup (__exposed); file_get_contents() also unbuffers its stream, but
 * never exposes it. */
static int phasync_stdio_mode(php_stream *stream, php_socket_t *fdp)
{
	zend_execute_data *ex = EG(current_execute_data);
	phasync_stdio_head *h = (phasync_stdio_head *) stream->abstract;

	if (!phasync_reading() || EG(active_fiber) == NULL
	 || stream->ops != &php_stream_stdio_ops || h->file != NULL || h->fd < 0
	 || ((stream->flags & PHP_STREAM_FLAG_NO_BUFFER) && stream->__exposed)
	 || ex == NULL || ex->func == NULL || ZEND_USER_CODE(ex->func->type)
	 || stream->readfilters.head || stream->writefilters.head) {
		return PHASYNC_FS_INLINE;
	}
	*fdp = h->fd;
	if (stream != PHASYNC_G(fs_last)) {
		PHASYNC_G(fs_last) = stream;         /* a file stays on its filesystem */
		PHASYNC_G(fs_last_mode) = phasync_fs_fd_mode(h->fd);
	}
	return PHASYNC_G(fs_last_mode);
}

static int phasync_stdio_set_option(php_stream *stream, int option, int value, void *ptrparam)
{
	/* fsync()/fdatasync() wait for the disk: inside a scope, in a fiber, on the
	 * pool. A stream with a C FILE* (its own buffer, to flush first on PHP's
	 * thread) stays native; plain file streams write straight to the fd. */
	if (option == PHP_STREAM_OPTION_SYNC_API
	 && (value == PHP_STREAM_SYNC_FSYNC || value == PHP_STREAM_SYNC_FDSYNC)
	 && phasync_reading() && EG(active_fiber) != NULL
	 && ((phasync_stdio_head *) stream->abstract)->file == NULL
	 && ((phasync_stdio_head *) stream->abstract)->fd >= 0) {
		phasync_task t;
		memset(&t, 0, sizeof(t));
		t.type = PHASYNC_OP_FS;
		t.fsop = value == PHP_STREAM_SYNC_FSYNC ? PHASYNC_FS_FSYNC : PHASYNC_FS_FDATASYNC;
		t.fd = ((phasync_stdio_head *) stream->abstract)->fd;
		if (!phasync_inflight_enter(stream)) {
			return PHP_STREAM_OPTION_RETURN_ERR;
		}
		phasync_pool_run(&t);
		phasync_inflight_leave(stream);
		if (EG(exception)) {
			return PHP_STREAM_OPTION_RETURN_ERR;
		}
		errno = t.err;
		return t.result == 0 ? PHP_STREAM_OPTION_RETURN_OK : PHP_STREAM_OPTION_RETURN_ERR;
	}
	/* flock() (and LOCK_EX in file_put_contents(), SplFileObject::flock()) blocks
	 * in flock(2) until the lock is free, with no descriptor to wait on. Inside a
	 * scope, in a fiber, try without blocking and sleep through the sleep handler
	 * between tries (1ms, doubling to 20ms). Plain and wrapped stdio streams both
	 * end up here. */
	if (option == PHP_STREAM_OPTION_LOCKING && ptrparam == NULL
	 && (value == LOCK_SH || value == LOCK_EX)
	 && phasync_sleep_handler() != NULL && EG(active_fiber) != NULL) {
		zend_long usec = 1000;
		int rc;
		while ((rc = phasync_stdio_set_option_orig(stream, option, value | LOCK_NB, NULL)) != 0
		       && errno == EWOULDBLOCK) {
			if (!phasync_inflight_enter(stream)) {
				return -1;
			}
			rc = phasync_call_sleep(phasync_sleep_handler(), usec);
			if (phasync_inflight_leave(stream) || rc < 0) {
				return -1;               /* closed meanwhile, or exception pending */
			}
			usec = MIN(usec * 2, 20000);
		}
		return rc;
	}
	php_socket_t fd;
	if (option == PHP_STREAM_OPTION_MMAP_API && value == PHP_STREAM_MMAP_SUPPORTED
	 && phasync_stdio_mode(stream, &fd) != PHASYNC_FS_INLINE) {
		return PHP_STREAM_OPTION_RETURN_NOTIMPL;   /* a page fault could block: read */
	}
	return phasync_stdio_set_option_orig(stream, option, value, ptrparam);
}

static ssize_t phasync_stdio_write(php_stream *stream, const char *buf, size_t count)
{
	php_socket_t fd;

	/* A buffered write waits for the disk only under writeback pressure, and
	 * RWF_NOWAIT would refuse any write that allocates blocks: only a pool file
	 * writes on the pool. */
	if (phasync_stdio_mode(stream, &fd) == PHASYNC_FS_POOL) {
		return phasync_pool_write_fd(stream, fd, buf, count);
	}
	return phasync_stdio_write_orig(stream, buf, count);
}

/* php_stream_stdio_ops.read/write, patched process-wide at MINIT, for streams
 * built internally, where there is no other hook: regular files (above), and the
 * pipe of php_exec()/shell_exec(). For those an exec-family override arms
 * PHASYNC_G(spawn); they read their popen() pipe right after spawning, before
 * anything else runs, so the first FIFO read then is that pipe: taint it with the
 * child's pidfd and wrap it. */
static ssize_t phasync_stdio_read(php_stream *stream, char *buf, size_t count)
{
	phasync_spawn *sp = PHASYNC_G(spawn);
	php_socket_t fd;
	struct stat st;

	if (sp && stream->ops == &php_stream_stdio_ops
	 && (fd = phasync_stream_fd(stream)) != -1 && fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode)) {
		PHASYNC_G(spawn) = NULL;
		phasync_entry_ensure(stream)->pidfd = phasync_new_child_pidfd(sp);
		phasync_wrap_stream(stream, PHASYNC_MODE_RAW);
		return stream->ops->read(stream, buf, count);
	}
	switch (phasync_stdio_mode(stream, &fd)) {
		case PHASYNC_FS_POOL:
			return phasync_pool_read_fd(stream, fd, buf, count);
#if PHASYNC_HAVE_NOWAIT
		case PHASYNC_FS_NOWAIT: {
			/* From the page cache only, at the current position (-1) as read()
			 * does. A short read returns as is: PHP reads a plain file greedily,
			 * so the next read asks for the rest, and at EOF finds 0 as it would
			 * natively. */
			struct iovec iov = { buf, count };
			ssize_t n = syscall(SYS_preadv2, fd, &iov, 1, -1L, -1L, RWF_NOWAIT);
			if (n >= 0) {
				if (n == 0) {
					stream->eof = 1;
				}
				php_clear_stat_cache(0, NULL, 0);   /* as the native read: atime changed */
				return n;
			}
			if (errno == EAGAIN) {
				/* Not cached: it would wait for the disk. Only a regular file
				 * reads on the pool (a pipe says EAGAIN when empty). */
				if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
					return phasync_pool_read_fd(stream, fd, buf, count);
				}
			} else if (errno == EOPNOTSUPP || errno == EINVAL || errno == ENOSYS) {
				/* This filesystem (or kernel) can't tell: its files read inline,
				 * as natively, and none of them tries again. */
				if (fstat(fd, &st) == 0) {
					dev_t *nd = realloc(PHASYNC_G(nowait_off), (PHASYNC_G(nnowait_off) + 1) * sizeof(dev_t));
					if (nd) {
						PHASYNC_G(nowait_off) = nd;
						nd[PHASYNC_G(nnowait_off)++] = st.st_dev;
					}
				}
				PHASYNC_G(fs_last_mode) = PHASYNC_FS_INLINE;
			}
			break;
		}
#endif
	}
	return phasync_stdio_read_orig(stream, buf, count);
}

static void phasync_exec_family(INTERNAL_FUNCTION_PARAMETERS, void (*orig)(INTERNAL_FUNCTION_PARAMETERS))
{
	phasync_spawn sp;

	if (!phasync_reading() || EG(active_fiber) == NULL) {
		orig(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	sp.n = phasync_children(sp.before);
	PHASYNC_G(spawn) = &sp;
	orig(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	PHASYNC_G(spawn) = NULL;             /* never left armed, whatever happened */
}

static ZEND_NAMED_FUNCTION(phasync_exec_override)
{
	phasync_exec_family(INTERNAL_FUNCTION_PARAM_PASSTHRU, PHASYNC_G(orig_exec));
}
static ZEND_NAMED_FUNCTION(phasync_system_override)
{
	phasync_exec_family(INTERNAL_FUNCTION_PARAM_PASSTHRU, PHASYNC_G(orig_system));
}
static ZEND_NAMED_FUNCTION(phasync_passthru_override)
{
	phasync_exec_family(INTERNAL_FUNCTION_PARAM_PASSTHRU, PHASYNC_G(orig_passthru));
}
static ZEND_NAMED_FUNCTION(phasync_shell_exec_override)
{
	phasync_exec_family(INTERNAL_FUNCTION_PARAM_PASSTHRU, PHASYNC_G(orig_shell_exec));
}

/* popen() streams are wrapped like proc_open() pipes, and tainted. */
static ZEND_NAMED_FUNCTION(phasync_popen_override)
{
	phasync_spawn sp;
	php_stream *s = NULL;

	sp.n = phasync_children(sp.before);
	PHASYNC_G(orig_popen)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	if (Z_TYPE_P(return_value) != IS_RESOURCE) {
		return;
	}
	php_stream_from_zval_no_verify(s, return_value);
	if (s) {
		phasync_entry_ensure(s)->pidfd = phasync_new_child_pidfd(&sp);
		phasync_wrap_stream(s, PHASYNC_MODE_RAW);
	}
}

/* ext/standard's php_process_handle: its leading fields, the same from PHP 8.2 to
 * master on POSIX. */
typedef struct {
	pid_t           child;
	int             npipes;
	zend_resource **pipes;
} phasync_proc_handle;

static ZEND_NAMED_FUNCTION(phasync_proc_close_override)
{
	int dir = phasync_reading();
	zval *zproc;
	const char *type;
	phasync_proc_handle *proc;
	siginfo_t si;
	int pidfd, i, w;

	if (!dir || EG(active_fiber) == NULL || ZEND_NUM_ARGS() != 1
	 || Z_TYPE_P(zproc = ZEND_CALL_ARG(execute_data, 1)) != IS_RESOURCE
	 || (type = zend_rsrc_list_get_rsrc_type(Z_RES_P(zproc))) == NULL || strcmp(type, "process") != 0) {
		PHASYNC_G(orig_proc_close)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	proc = Z_RES_P(zproc)->ptr;
	/* A coroutine being destroyed cannot wait for another coroutine still inside
	 * an op on one of the pipes (#23): native proc_close() would free a pipe's
	 * stream under it exactly as fclose() would (its own zend_list_close(), just
	 * below, does not wait for other references either). Wake it, as
	 * phasync_close_if_unwinding() does for fclose()/pclose(), and leave the
	 * whole process handle where it is -- to whichever reference (the pipe's, or
	 * $proc itself) is destroyed last, at which point closing it goes through
	 * the same path with nothing left to wait for. */
	if (phasync_unwinding()) {
		bool deferred = false;
		for (i = 0; i < proc->npipes; i++) {
			php_stream *pipe_stream;
			if (proc->pipes[i] != NULL
			 && (pipe_stream = zend_fetch_resource2(proc->pipes[i], NULL, php_file_le_stream(), php_file_le_pstream()))
			 && phasync_close_if_unwinding(pipe_stream)) {
				deferred = true;
			}
		}
		if (deferred) {
			RETURN_LONG(-1);
		}
	}
	/* First close the pipes, as the native close does, so a child waiting for EOF
	 * on its stdin can exit. */
	for (i = 0; i < proc->npipes; i++) {
		if (proc->pipes[i] != NULL) {
			GC_DELREF(proc->pipes[i]);
			zend_list_close(proc->pipes[i]);
			proc->pipes[i] = NULL;
		}
	}
	/* waitid(WNOWAIT) checks the pid is still our child (proc_get_status() may have
	 * reaped it); then the pidfd turns readable when it exits, and the original
	 * reaps it at once. A coroutine being destroyed cannot wait here either: skip
	 * straight to the native, blocking reap (#23; up to here this override simply
	 * never ran to completion for such a coroutine, an adjacent bug filed as its
	 * own issue). */
	pidfd = phasync_unwinding() ? -1 : phasync_proc_fd(proc->child);
	if (pidfd >= 0) {
		w = waitid(P_PID, proc->child, &si, WEXITED | WNOHANG | WNOWAIT) == 0
			? phasync_wait_fd(dir, NULL, pidfd, INFINITY) : PHASYNC_WAIT_READY;
		close(pidfd);
		if (w == PHASYNC_WAIT_ERROR) {
			return;                      /* exception pending: propagate */
		}
	}
	PHASYNC_G(orig_proc_close)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

/* pcntl_waitpid()/pcntl_wait() block until a child changes state. Inside a
 * scope, in a fiber, for an exit (no WNOHANG/WUNTRACED/WCONTINUED): a given pid
 * that is still an unreaped child of ours is waited for on a pidfd, readable
 * once it exits; any child or a process group (pid <= 0) has no pidfd, so it is
 * checked without reaping (waitid(WNOWAIT)) and slept on between checks (1ms,
 * doubling to 20ms). Then the original reaps at once, with the native status
 * and resource usage. */
static void phasync_waitpid_common(INTERNAL_FUNCTION_PARAMETERS, zend_long pid, zend_long flags,
                                   void (*orig)(INTERNAL_FUNCTION_PARAMETERS))
{
	siginfo_t si;
	int pidfd;

	if (!phasync_reading() || EG(active_fiber) == NULL
	 || (flags & (WNOHANG | WUNTRACED
#ifdef WCONTINUED
	              | WCONTINUED
#endif
	 ))) {
		orig(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	if (pid > 0) {
		if ((pidfd = phasync_proc_fd((pid_t) pid)) >= 0) {
			int w = PHASYNC_WAIT_READY;
			memset(&si, 0, sizeof(si));
			if (waitid(P_PID, (id_t) pid, &si, WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid == 0) {
				w = phasync_wait_fd(PHASYNC_READ, NULL, pidfd, INFINITY);   /* not exited yet */
			}
			close(pidfd);
			if (w != PHASYNC_WAIT_READY) {
				return;                      /* exception pending */
			}
		}
	} else {
		idtype_t type = pid == -1 ? P_ALL : P_PGID;
		id_t id = pid == -1 ? 0 : pid == 0 ? (id_t) getpgrp() : (id_t) -pid;
		zend_long usec = 1000;
		for (;;) {
			memset(&si, 0, sizeof(si));
			if (waitid(type, id, &si, WEXITED | WNOHANG | WNOWAIT) != 0 || si.si_pid != 0) {
				break;                       /* one exited, or none to wait for (ECHILD) */
			}
			if (phasync_call_sleep(phasync_sleep_handler(), usec) < 0) {
				return;                      /* exception pending */
			}
			usec = MIN(usec * 2, 20000);
		}
	}
	orig(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

static ZEND_NAMED_FUNCTION(phasync_pcntl_waitpid_override)
{
	uint32_t argc = ZEND_NUM_ARGS();
	zval *zpid = argc >= 1 ? ZEND_CALL_ARG(execute_data, 1) : NULL;
	zval *zflags = argc >= 3 ? ZEND_CALL_ARG(execute_data, 3) : NULL;

	if (!zpid || Z_TYPE_P(zpid) != IS_LONG || (zflags && Z_TYPE_P(zflags) != IS_LONG)) {
		PHASYNC_G(orig_pcntl_waitpid)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	phasync_waitpid_common(INTERNAL_FUNCTION_PARAM_PASSTHRU, Z_LVAL_P(zpid), zflags ? Z_LVAL_P(zflags) : 0,
		PHASYNC_G(orig_pcntl_waitpid));
}

static ZEND_NAMED_FUNCTION(phasync_pcntl_wait_override)
{
	uint32_t argc = ZEND_NUM_ARGS();
	zval *zflags = argc >= 2 ? ZEND_CALL_ARG(execute_data, 2) : NULL;

	if (zflags && Z_TYPE_P(zflags) != IS_LONG) {
		PHASYNC_G(orig_pcntl_wait)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	phasync_waitpid_common(INTERNAL_FUNCTION_PARAM_PASSTHRU, -1, zflags ? Z_LVAL_P(zflags) : 0,
		PHASYNC_G(orig_pcntl_wait));
}

/* ---- enable_hooks / disable_hooks ---------------------------------------- */

/* ---- libmemcached and libpq: their poll() and getaddrinfo() --------------
 *
 * libmemcached (php-memcached) and libpq (pgsql, pdo_pgsql) do all their I/O on
 * non-blocking sockets and wait in exactly two places: libc poll() and, for a
 * host name, getaddrinfo(). Both calls are redirected by rewriting the libraries'
 * own GOT entries for them at startup: inside a coroutine the replacement parks
 * the coroutine instead of blocking the process, everywhere else it is the libc
 * call. That covers every call, TLS included, with no per-function code, and the
 * bytes on the wire, the results and the errors stay the library's own. One call
 * at a time per connection: see phasync_conn_call(). */

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
/* poll() as the libraries call it, inside a coroutine: the same answer, reached by
 * parking until a descriptor is ready or the timeout is up rather than blocking.
 * The descriptors go to a private epoll instance, so a poll over several of them
 * is one wait. Whatever happens in between, the answer is a poll() with a zero
 * timeout: a wake-up that finds nothing ready just waits again. */
static int phasync_got_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	double deadline;
	int rc;

	if (timeout == 0 || !phasync_reading() || EG(active_fiber) == NULL || phasync_unwinding()) {
		return poll(fds, nfds, timeout);
	}
	deadline = timeout < 0 ? INFINITY : phasync_now() + timeout / 1000.0;
	while ((rc = poll(fds, nfds, 0)) == 0) {
		double left = deadline - phasync_now();
		int epfd, w;

		if (left <= 0) {
			break;
		}
		if ((epfd = phasync_mux_open()) < 0) {
			return -1;
		}
		for (nfds_t i = 0; i < nfds; i++) {
			phasync_mux_add(epfd, fds[i].fd, (fds[i].events & POLLIN ? PHASYNC_EV_IN : 0)
			                | (fds[i].events & POLLPRI ? PHASYNC_EV_PRI : 0) | (fds[i].events & POLLOUT ? PHASYNC_EV_OUT : 0));
		}
		w = phasync_wait_fd(PHASYNC_READ, NULL, epfd, left);
		close(epfd);
		if (w == PHASYNC_WAIT_ERROR) {
			errno = ECANCELED;                  /* the exception pending propagates */
			return -1;
		}
	}
	return rc;
}

/* getaddrinfo() as the libraries call it: a host name is resolved on the thread
 * pool, as the stream connect does; an address needs no lookup. */
static int phasync_got_getaddrinfo(const char *node, const char *service,
                                    const struct addrinfo *hints, struct addrinfo **res)
{
	unsigned char bin[sizeof(struct in6_addr)];
	phasync_task t;

	if (!phasync_reading() || EG(active_fiber) == NULL || phasync_unwinding()
	 || inet_pton(AF_INET, node, bin) == 1 || inet_pton(AF_INET6, node, bin) == 1) {
		return getaddrinfo(node, service, hints, res);
	}
	memset(&t, 0, sizeof(t));
	t.type  = PHASYNC_OP_GETADDRINFO;
	t.host  = node;
	t.hints = *hints;
	snprintf(t.serv, sizeof(t.serv), "%s", service);
	phasync_pool_run(&t);
	if (EG(exception)) {
		if (t.ai) {
			freeaddrinfo(t.ai);
		}
		errno = ECANCELED;
		return EAI_SYSTEM;
	}
	*res = t.ai;
	return (int) t.result;
}

# include <link.h>
# include <sys/mman.h>

static struct { void **slot; void *orig; bool relro; } phasync_got_patched[16];
static int phasync_got_n;

static void phasync_got_write(void **slot, void *value, bool relro)
{
	long pg = getpagesize();
	void *page = (void *) ((uintptr_t) slot & ~(uintptr_t) (pg - 1));

	mprotect(page, pg, PROT_READ | PROT_WRITE);
	*slot = value;
	if (relro) {
		mprotect(page, pg, PROT_READ);
	}
}

/* Point every relocation of libmemcached.so and libpq.so for poll and getaddrinfo at ours. */
static int phasync_got_object(struct dl_phdr_info *info, size_t size, void *arg)
{
	const char *name = strrchr(info->dlpi_name, '/');
	const ElfW(Dyn) *dyn = NULL, *d;
	const ElfW(Rela) *rel[2] = { NULL, NULL };
	size_t relsz[2] = { 0, 0 };
	const ElfW(Sym) *symtab = NULL;
	const char *strtab = NULL;
	ElfW(Addr) relro_lo = 0, relro_hi = 0;

	name = name ? name + 1 : info->dlpi_name;
	if (strncmp(name, "libmemcached.so", sizeof("libmemcached.so") - 1) != 0
	 && strncmp(name, "libpq.so", sizeof("libpq.so") - 1) != 0) {
		return 0;
	}
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
		if (ph->p_type == PT_DYNAMIC) {
			dyn = (const ElfW(Dyn) *) (info->dlpi_addr + ph->p_vaddr);
		} else if (ph->p_type == PT_GNU_RELRO) {
			relro_lo = info->dlpi_addr + ph->p_vaddr;
			relro_hi = relro_lo + ph->p_memsz;
		}
	}
	/* glibc has relocated the pointers in .dynamic already, musl has not. */
#define PHASYNC_DYN_PTR(d) ((d)->d_un.d_ptr < info->dlpi_addr ? info->dlpi_addr + (d)->d_un.d_ptr : (d)->d_un.d_ptr)
	for (d = dyn; d->d_tag != DT_NULL; d++) {
		switch (d->d_tag) {
			case DT_SYMTAB:   symtab = (const ElfW(Sym) *) PHASYNC_DYN_PTR(d); break;
			case DT_STRTAB:   strtab = (const char *) PHASYNC_DYN_PTR(d); break;
			case DT_JMPREL:   rel[0] = (const ElfW(Rela) *) PHASYNC_DYN_PTR(d); break;
			case DT_PLTRELSZ: relsz[0] = d->d_un.d_val; break;
			case DT_RELA:     rel[1] = (const ElfW(Rela) *) PHASYNC_DYN_PTR(d); break;
			case DT_RELASZ:   relsz[1] = d->d_un.d_val; break;
		}
	}
#undef PHASYNC_DYN_PTR
	for (int t = 0; t < 2; t++) {
		for (size_t i = 0; rel[t] && i < relsz[t] / sizeof(ElfW(Rela)); i++) {
			const char *sym = strtab + symtab[ELF64_R_SYM(rel[t][i].r_info)].st_name;
			void **slot = (void **) (info->dlpi_addr + rel[t][i].r_offset);
			void *repl = strcmp(sym, "poll") == 0 ? (void *) phasync_got_poll
			           : strcmp(sym, "getaddrinfo") == 0 ? (void *) phasync_got_getaddrinfo : NULL;

			if (repl) {
				bool relro = (ElfW(Addr)) slot >= relro_lo && (ElfW(Addr)) slot < relro_hi;

				phasync_got_patched[phasync_got_n].slot = slot;
				phasync_got_patched[phasync_got_n].orig = *slot;
				phasync_got_patched[phasync_got_n++].relro = relro;
				phasync_got_write(slot, repl, relro);
			}
		}
	}
	return 0;
}

static void phasync_got_patch(void)
{
	dl_iterate_phdr(phasync_got_object, NULL);
}

/* Before this module is unloaded: libmemcached and libpq may still run (their
 * persistent handles are freed by their extensions' own shutdown). */
static void phasync_got_restore(void)
{
	for (int i = 0; i < phasync_got_n; i++) {
		phasync_got_write(phasync_got_patched[i].slot, phasync_got_patched[i].orig, phasync_got_patched[i].relro);
	}
	phasync_got_n = 0;
}
#else
static void phasync_got_patch(void) {}
static void phasync_got_restore(void) {}
#endif

/* ---- one call at a time on a connection ------------------------------------
 *
 * A connection (a libmemcached handle, a PGconn, a SQLite3 or PDO database) is in
 * one call at a time: a second one entering it while the first is suspended would
 * interleave on its socket, or free it under the first. So, inside a scope, every
 * method or function on such a connection marks it busy for the duration of the
 * call, and a call from another coroutine entering a busy connection throws. The
 * coroutine in the call may enter it again (a callback calling the object). The
 * original handler is kept in the function's reserved slot, so the copies PHP
 * makes of inherited internal methods find it too. */

static int phasync_conn_slot;
static void *phasync_pgsql_globals;
static zend_class_entry *phasync_sqlite3_ce[2];   /* SQLite3Stmt, SQLite3Result */

/* pgsql's globals and the SQLite3 objects' members are read at positions known
 * for PHP 8.3-8.6. Elsewhere those two are not hooked: stock behaviour, which
 * phpinfo() states. */
static bool phasync_test_unknown_layouts;         /* phasync.test_unknown_layouts (test builds) */
static bool phasync_conn_pgsql_on, phasync_conn_sqlite3_on;

static bool phasync_layouts_known(void)
{
	return PHP_VERSION_ID >= 80300 && PHP_VERSION_ID < 80700 && !phasync_test_unknown_layouts;
}

/* Enter a connection: 1 entered, 0 nothing to do (none, or the caller is in it
 * already), -1 another coroutine is (thrown). */
static int phasync_conn_enter(zend_ulong key)
{
	void *busy;

	if (key == 0) {
		return 0;
	}
	if ((busy = zend_hash_index_find_ptr(&PHASYNC_G(conn_busy), key)) == NULL) {
		zend_hash_index_add_new_ptr(&PHASYNC_G(conn_busy), key, EG(current_fiber_context));
		return 1;
	}
	if (busy == EG(current_fiber_context)) {
		return 0;
	}
	zend_throw_exception(spl_ce_LogicException, "Concurrent operations on the same resource are not supported", 0);
	return -1;
}

/* Call the original inside the connection(s) key and key2 (0: none). */
static void phasync_conn_call2(zend_ulong key, zend_ulong key2, INTERNAL_FUNCTION_PARAMETERS)
{
	zif_handler orig = EX(func)->internal_function.reserved[phasync_conn_slot];
	int in = 0, in2 = 0;

	if (phasync_reading() && ((in = phasync_conn_enter(key)) < 0 || (in2 = phasync_conn_enter(key2)) < 0)) {
		if (in > 0) {
			zend_hash_index_del(&PHASYNC_G(conn_busy), key);
		}
		return;
	}
	orig(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	if (in > 0) {
		zend_hash_index_del(&PHASYNC_G(conn_busy), key);
	}
	if (in2 > 0) {
		zend_hash_index_del(&PHASYNC_G(conn_busy), key2);
	}
}

static void phasync_conn_call(void *conn, INTERNAL_FUNCTION_PARAMETERS)
{
	phasync_conn_call2((zend_ulong) (uintptr_t) conn, 0, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

/* Memcached: the libmemcached handle, the first member of php-memcached's object
 * (persistent_id shares one handle between objects). */
static ZEND_NAMED_FUNCTION(phasync_conn_memcached)
{
	zend_object *obj = Z_OBJ_P(ZEND_THIS);

	phasync_conn_call(*(void **) ((char *) obj - obj->handlers->offset), INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

/* pg_*(): the PGconn of the first PgSql\Connection, Result or Lob argument (the
 * first member of each), else of the default link. pgsql's globals up to it: */
typedef struct {
	zend_long num_links, num_persistent, max_links, max_persistent;
#if PHP_VERSION_ID < 80500
	bool allow_persistent;
	int ignore_notices;
	zend_long auto_reset_persistent;
	int log_notices;
#else
	zend_long auto_reset_persistent;
#endif
	zend_object *default_link;
} phasync_pgsql_globals_head;
#define PHASYNC_PGSQL_CONNECT_FORCE_NEW 2

static ZEND_NAMED_FUNCTION(phasync_conn_pgsql)
{
	zend_object *obj = ((phasync_pgsql_globals_head *) phasync_pgsql_globals)->default_link;

	for (uint32_t i = 1; i <= ZEND_NUM_ARGS(); i++) {
		zval *a = ZEND_CALL_ARG(execute_data, i);

		if (Z_TYPE_P(a) == IS_OBJECT && Z_OBJCE_P(a)->type == ZEND_INTERNAL_CLASS
		 && Z_OBJCE_P(a)->info.internal.module == EX(func)->internal_function.module) {
			obj = Z_OBJ_P(a);
			break;
		}
	}
	phasync_conn_call(obj ? *(void **) ((char *) obj - obj->handlers->offset) : NULL, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

/* pg_pconnect() enters its persistent slot (a connection made concurrently
 * would replace, and free, the other's), and the PGconn it reuses and talks on
 * ("RESET ALL"), found as pgsql's own code finds it. A slot's key is odd: no
 * connection's address. */
static ZEND_NAMED_FUNCTION(phasync_conn_pgsql_pconnect)
{
	zval *cs = ZEND_NUM_ARGS() >= 1 ? ZEND_CALL_ARG(execute_data, 1) : NULL;
	zval *ct = ZEND_NUM_ARGS() >= 2 ? ZEND_CALL_ARG(execute_data, 2) : NULL;
	zend_resource *le = NULL;
	zend_ulong slot = 0;

	if (cs && Z_TYPE_P(cs) == IS_STRING && (!ct || Z_TYPE_P(ct) == IS_LONG)) {
		zend_string *key = zend_strpprintf(0, "pgsql%s_" ZEND_LONG_FMT, Z_STRVAL_P(cs), ct ? Z_LVAL_P(ct) & ~PHASYNC_PGSQL_CONNECT_FORCE_NEW : 0);

		le = zend_hash_find_ptr(&EG(persistent_list), key);
		slot = ZSTR_HASH(key) | 1;
		zend_string_release(key);
	}
	phasync_conn_call2(slot, le ? (zend_ulong) (uintptr_t) le->ptr : 0, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

/* SQLite3: its object; SQLite3Stmt and SQLite3Result point to it (their second
 * and first member). */
static ZEND_NAMED_FUNCTION(phasync_conn_sqlite3)
{
	zend_object *obj = Z_OBJ_P(ZEND_THIS);
	void **o = (void **) ((char *) obj - obj->handlers->offset);
	zend_class_entry *scope = EX(func)->common.scope;

	phasync_conn_call(scope == phasync_sqlite3_ce[0] ? o[1] : scope == phasync_sqlite3_ce[1] ? o[0] : (void *) o,
		INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

#ifdef PHASYNC_HAVE_PDO
/* PDO and PDOStatement: the pdo_dbh_t (persistent PDO objects share one), for
 * the pgsql and sqlite drivers. */
static void *phasync_pdo_conn(pdo_dbh_t *dbh)
{
	return dbh && dbh->driver && (strcmp(dbh->driver->driver_name, "pgsql") == 0
	                           || strcmp(dbh->driver->driver_name, "sqlite") == 0) ? dbh : NULL;
}

static ZEND_NAMED_FUNCTION(phasync_conn_pdo)
{
	phasync_conn_call(phasync_pdo_conn(php_pdo_dbh_fetch_inner(Z_OBJ_P(ZEND_THIS))), INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

static ZEND_NAMED_FUNCTION(phasync_conn_pdostmt)
{
	phasync_conn_call(phasync_pdo_conn(php_pdo_stmt_fetch_object(Z_OBJ_P(ZEND_THIS))->dbh), INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
#endif

/* Route a function through handler, remembering its original. */
static void phasync_conn_hook(zend_function *f, zif_handler handler)
{
	if (f->type != ZEND_INTERNAL_FUNCTION || (f->common.fn_flags & ZEND_ACC_STATIC)
	 || !zend_hash_index_add_empty_element(&PHASYNC_G(conn_hooks), (zend_ulong) (uintptr_t) f)) {
		return;
	}
	f->internal_function.reserved[phasync_conn_slot] = (void *) f->internal_function.handler;
	f->internal_function.handler = handler;
}

static void phasync_conn_hook_class(const char *name, size_t len, zif_handler handler)
{
	zend_class_entry *ce = zend_hash_str_find_ptr(CG(class_table), name, len);
	zend_function *f;

	if (ce) {
		ZEND_HASH_FOREACH_PTR(&ce->function_table, f) {
			phasync_conn_hook(f, handler);
		} ZEND_HASH_FOREACH_END();
	}
}

static void phasync_conn_hooks_install(void)
{
	zend_module_entry *pgsql = zend_hash_str_find_ptr(&module_registry, ZEND_STRL("pgsql"));
	zend_function *f;

	if (phasync_conn_slot < 0) {
		return;                          /* no reserved slot left: stock behaviour */
	}
	phasync_conn_hook_class(ZEND_STRL("memcached"), phasync_conn_memcached);
	if ((phasync_conn_sqlite3_on = phasync_layouts_known() && zend_hash_str_exists(CG(class_table), ZEND_STRL("sqlite3")))) {
		phasync_conn_hook_class(ZEND_STRL("sqlite3"), phasync_conn_sqlite3);
		phasync_conn_hook_class(ZEND_STRL("sqlite3stmt"), phasync_conn_sqlite3);
		phasync_conn_hook_class(ZEND_STRL("sqlite3result"), phasync_conn_sqlite3);
		phasync_sqlite3_ce[0] = zend_hash_str_find_ptr(CG(class_table), ZEND_STRL("sqlite3stmt"));
		phasync_sqlite3_ce[1] = zend_hash_str_find_ptr(CG(class_table), ZEND_STRL("sqlite3result"));
	}
#ifdef PHASYNC_HAVE_PDO
	phasync_conn_hook_class(ZEND_STRL("pdo"), phasync_conn_pdo);
	phasync_conn_hook_class(ZEND_STRL("pdo\\pgsql"), phasync_conn_pdo);
	phasync_conn_hook_class(ZEND_STRL("pdo\\sqlite"), phasync_conn_pdo);
	phasync_conn_hook_class(ZEND_STRL("pdostatement"), phasync_conn_pdostmt);
#endif
	if ((phasync_conn_pgsql_on = phasync_layouts_known() && pgsql && pgsql->module_started
	                           && pgsql->globals_size >= sizeof(phasync_pgsql_globals_head))) {
#ifdef ZTS
		phasync_pgsql_globals = TSRMG_BULK(*pgsql->globals_id_ptr, void *);
#else
		phasync_pgsql_globals = pgsql->globals_ptr;
#endif
		ZEND_HASH_MAP_FOREACH_PTR(CG(function_table), f) {
			if (f->type == ZEND_INTERNAL_FUNCTION && f->internal_function.module == pgsql
			 && !zend_string_equals_literal(f->common.function_name, "pg_connect")) {
				phasync_conn_hook(f, zend_string_equals_literal(f->common.function_name, "pg_pconnect")
					? phasync_conn_pgsql_pconnect : phasync_conn_pgsql);
			}
		} ZEND_HASH_FOREACH_END();
	}
}

static void phasync_conn_hooks_restore(void)
{
	zend_ulong f;

	ZEND_HASH_FOREACH_NUM_KEY(&PHASYNC_G(conn_hooks), f) {
		zend_internal_function *fn = &((zend_function *) (uintptr_t) f)->internal_function;
		fn->handler = (zif_handler) fn->reserved[phasync_conn_slot];
	} ZEND_HASH_FOREACH_END();
	zend_hash_clean(&PHASYNC_G(conn_hooks));
	zend_hash_clean(&PHASYNC_G(conn_busy));
}

static zend_internal_function *phasync_find_ifunc(const char *name, size_t len)
{
	zend_function *f = zend_hash_str_find_ptr(CG(function_table), name, len);
	if (f && f->type == ZEND_INTERNAL_FUNCTION) {
		return &f->internal_function;
	}
	return NULL;
}

/* Install the transport re-registration + internal-function overrides. Called
 * lazily on the first manage() of a request and left in place: the hooks are
 * inert while no scope is active (the wrapped ops and sleep/pool overrides fall
 * through to the original when scope_top is NULL), so there is nothing to toggle
 * off. Idempotent. */
static void phasync_install_hooks(void)
{
	HashTable *xhash;
	zend_internal_function *f;

	if (PHASYNC_G(hooks_installed)) {
		return;
	}

	xhash = php_stream_xport_get_hash();
	PHASYNC_G(orig_tcp)  = zend_hash_str_find_ptr(xhash, "tcp", sizeof("tcp") - 1);
	PHASYNC_G(orig_unix) = zend_hash_str_find_ptr(xhash, "unix", sizeof("unix") - 1);
	if (PHASYNC_G(orig_tcp))  php_stream_xport_register("tcp", phasync_tcp_factory);
	if (PHASYNC_G(orig_unix)) php_stream_xport_register("unix", phasync_unix_factory);
	PHASYNC_G(orig_udp) = zend_hash_str_find_ptr(xhash, "udp", sizeof("udp") - 1);
	PHASYNC_G(orig_udg) = zend_hash_str_find_ptr(xhash, "udg", sizeof("udg") - 1);
	if (PHASYNC_G(orig_udp)) php_stream_xport_register("udp", phasync_udp_factory);
	if (PHASYNC_G(orig_udg)) php_stream_xport_register("udg", phasync_udg_factory);

	PHASYNC_G(orig_ssl) = zend_hash_str_find_ptr(xhash, "ssl", sizeof("ssl") - 1);
	if (PHASYNC_G(orig_ssl)) {
		const char **scheme;
		for (scheme = phasync_ssl_schemes; *scheme; scheme++) {
			if (zend_hash_str_exists(xhash, *scheme, strlen(*scheme))) {
				php_stream_xport_register(*scheme, phasync_ssl_factory);
			}
		}
	}

	if ((f = phasync_find_ifunc("proc_open", sizeof("proc_open") - 1))) {
		PHASYNC_G(orig_proc_open) = f->handler;
		f->handler = phasync_proc_open_override;
	}
	if ((f = phasync_find_ifunc("stream_socket_pair", sizeof("stream_socket_pair") - 1))) {
		PHASYNC_G(orig_stream_socket_pair) = f->handler;
		f->handler = phasync_stream_socket_pair_override;
	}
	if ((f = phasync_find_ifunc("popen", sizeof("popen") - 1))) {
		PHASYNC_G(orig_popen) = f->handler;
		f->handler = phasync_popen_override;
	}
	if ((f = phasync_find_ifunc("exec", sizeof("exec") - 1))) {
		PHASYNC_G(orig_exec) = f->handler;
		f->handler = phasync_exec_override;
	}
	if ((f = phasync_find_ifunc("system", sizeof("system") - 1))) {
		PHASYNC_G(orig_system) = f->handler;
		f->handler = phasync_system_override;
	}
	if ((f = phasync_find_ifunc("passthru", sizeof("passthru") - 1))) {
		PHASYNC_G(orig_passthru) = f->handler;
		f->handler = phasync_passthru_override;
	}
	if ((f = phasync_find_ifunc("shell_exec", sizeof("shell_exec") - 1))) {
		PHASYNC_G(orig_shell_exec) = f->handler;
		f->handler = phasync_shell_exec_override;
	}
	if ((f = phasync_find_ifunc("proc_close", sizeof("proc_close") - 1))) {
		PHASYNC_G(orig_proc_close) = f->handler;
		f->handler = phasync_proc_close_override;
	}
	if ((f = phasync_find_ifunc("curl_exec", sizeof("curl_exec") - 1))) {
		PHASYNC_G(orig_curl_exec) = f->handler;
		f->handler = phasync_curl_exec_override;
	}
	if ((f = phasync_find_ifunc("sem_acquire", sizeof("sem_acquire") - 1))) {
		PHASYNC_G(orig_sem_acquire) = f->handler;
		f->handler = phasync_sem_acquire_override;
	}
	if ((f = phasync_find_ifunc("msg_receive", sizeof("msg_receive") - 1))) {
		PHASYNC_G(orig_msg_receive) = f->handler;
		f->handler = phasync_msg_receive_override;
	}
	if ((f = phasync_find_ifunc("curl_multi_select", sizeof("curl_multi_select") - 1))) {
		PHASYNC_G(orig_curl_multi_select) = f->handler;
		f->handler = phasync_curl_multi_select_override;
	}
	if ((f = phasync_find_ifunc("socket_connect", sizeof("socket_connect") - 1))) {
		PHASYNC_G(orig_socket_connect) = f->handler;
		f->handler = phasync_socket_connect_override;
	}
	if ((f = phasync_find_ifunc("socket_close", sizeof("socket_close") - 1))) {
		PHASYNC_G(orig_socket_close) = f->handler;
		f->handler = phasync_socket_close_override;
	}
	for (zend_long i = 0; i < (zend_long) PHASYNC_SOCK_NFUNCS; i++) {
		if ((f = phasync_find_ifunc(phasync_sock_funcs[i].name, strlen(phasync_sock_funcs[i].name)))) {
			zval zi;
			ZVAL_LONG(&zi, i);
			PHASYNC_G(orig_sock)[i] = f->handler;
			f->handler = phasync_socket_io_override;
			zend_hash_index_update(&PHASYNC_G(sock_hooks), (zend_ulong) (uintptr_t) f->function_name, &zi);
		}
	}
	if ((f = phasync_find_ifunc("pcntl_waitpid", sizeof("pcntl_waitpid") - 1))) {
		PHASYNC_G(orig_pcntl_waitpid) = f->handler;
		f->handler = phasync_pcntl_waitpid_override;
	}
	if ((f = phasync_find_ifunc("pcntl_wait", sizeof("pcntl_wait") - 1))) {
		PHASYNC_G(orig_pcntl_wait) = f->handler;
		f->handler = phasync_pcntl_wait_override;
	}
	for (zend_long i = 0; i < (zend_long) PHASYNC_FS_NFUNCS; i++) {
		if ((f = phasync_find_ifunc(phasync_fs_funcs[i].name, strlen(phasync_fs_funcs[i].name)))) {
			zval zi;
			ZVAL_LONG(&zi, i);
			PHASYNC_G(orig_fs)[i] = f->handler;
			f->handler = phasync_fs_override;
			zend_hash_index_update(&PHASYNC_G(fs_hooks), (zend_ulong) (uintptr_t) f->function_name, &zi);
		}
	}
	if ((f = phasync_find_ifunc("stream_select", sizeof("stream_select") - 1))) {
		PHASYNC_G(orig_stream_select) = f->handler;
		f->handler = phasync_stream_select_override;
	}
	if ((f = phasync_find_ifunc("socket_select", sizeof("socket_select") - 1))) {
		PHASYNC_G(orig_socket_select) = f->handler;
		f->handler = phasync_socket_select_override;
	}
	if ((f = phasync_find_ifunc("sleep", sizeof("sleep") - 1))) {
		PHASYNC_G(orig_sleep) = f->handler;
		f->handler = phasync_sleep_override;
	}
	if ((f = phasync_find_ifunc("header_register_callback", sizeof("header_register_callback") - 1))) {
		PHASYNC_G(orig_header_register_callback) = f->handler;
		f->handler = phasync_header_register_callback_override;
	}
	if ((f = phasync_find_ifunc("usleep", sizeof("usleep") - 1))) {
		PHASYNC_G(orig_usleep) = f->handler;
		f->handler = phasync_usleep_override;
	}
	if ((f = phasync_find_ifunc("time_nanosleep", sizeof("time_nanosleep") - 1))) {
		PHASYNC_G(orig_time_nanosleep) = f->handler;
		f->handler = phasync_time_nanosleep_override;
	}
	if ((f = phasync_find_ifunc("time_sleep_until", sizeof("time_sleep_until") - 1))) {
		PHASYNC_G(orig_time_sleep_until) = f->handler;
		f->handler = phasync_time_sleep_until_override;
	}
	if ((f = phasync_find_ifunc("gethostbyname", sizeof("gethostbyname") - 1))) {
		PHASYNC_G(orig_gethostbyname) = f->handler;
		f->handler = phasync_gethostbyname_override;
	}
	if ((f = phasync_find_ifunc("gethostbynamel", sizeof("gethostbynamel") - 1))) {
		PHASYNC_G(orig_gethostbynamel) = f->handler;
		f->handler = phasync_gethostbynamel_override;
	}
	if ((f = phasync_find_ifunc("gethostbyaddr", sizeof("gethostbyaddr") - 1))) {
		PHASYNC_G(orig_gethostbyaddr) = f->handler;
		f->handler = phasync_gethostbyaddr_override;
	}
#ifdef HAVE_FULL_DNS_FUNCS
	{
		/* checkdnsrr/getmxrr are separate function-table entries (aliases). */
		static const struct { const char *name; int which; } dnsfns[] = {
			{ "dns_check_record", 0 }, { "checkdnsrr", 0 },
			{ "dns_get_record", 1 },
			{ "dns_get_mx", 2 }, { "getmxrr", 2 },
		};
		for (size_t k = 0; k < sizeof(dnsfns) / sizeof(dnsfns[0]); k++) {
			if ((f = phasync_find_ifunc(dnsfns[k].name, strlen(dnsfns[k].name)))) {
				switch (dnsfns[k].which) {
					case 0: PHASYNC_G(orig_dns_check_record) = f->handler; f->handler = phasync_dns_check_record_override; break;
					case 1: PHASYNC_G(orig_dns_get_record)   = f->handler; f->handler = phasync_dns_get_record_override;   break;
					case 2: PHASYNC_G(orig_dns_get_mx)       = f->handler; f->handler = phasync_dns_get_mx_override;       break;
				}
			}
		}
	}
#endif
	if ((f = phasync_find_ifunc("fopen", sizeof("fopen") - 1))) {
		PHASYNC_G(orig_fopen) = f->handler;
		f->handler = phasync_fopen_override;
	}
	if ((f = phasync_find_ifunc("fclose", sizeof("fclose") - 1))) {
		PHASYNC_G(orig_fclose) = f->handler;
		f->handler = phasync_fclose_override;
	}
	if ((f = phasync_find_ifunc("pclose", sizeof("pclose") - 1))) {
		PHASYNC_G(orig_pclose) = f->handler;
		f->handler = phasync_pclose_override;
	}
	phasync_conn_hooks_install();
	PHASYNC_G(hooks_installed) = 1;
}

static void phasync_restore_hooks(void)
{
	zend_internal_function *f;

	if (!PHASYNC_G(hooks_installed)) {
		return;
	}
	if (PHASYNC_G(orig_tcp))  php_stream_xport_register("tcp", PHASYNC_G(orig_tcp));
	if (PHASYNC_G(orig_unix)) php_stream_xport_register("unix", PHASYNC_G(orig_unix));
	if (PHASYNC_G(orig_udp))  php_stream_xport_register("udp", PHASYNC_G(orig_udp));
	if (PHASYNC_G(orig_udg))  php_stream_xport_register("udg", PHASYNC_G(orig_udg));
	if (PHASYNC_G(orig_ssl)) {
		HashTable *xh = php_stream_xport_get_hash();
		const char **scheme;
		for (scheme = phasync_ssl_schemes; *scheme; scheme++) {
			if (zend_hash_str_exists(xh, *scheme, strlen(*scheme))) {
				php_stream_xport_register(*scheme, PHASYNC_G(orig_ssl));
			}
		}
	}

	if (PHASYNC_G(orig_proc_open) && (f = phasync_find_ifunc("proc_open", sizeof("proc_open") - 1))) {
		f->handler = PHASYNC_G(orig_proc_open);
	}
	if (PHASYNC_G(orig_stream_socket_pair) && (f = phasync_find_ifunc("stream_socket_pair", sizeof("stream_socket_pair") - 1))) {
		f->handler = PHASYNC_G(orig_stream_socket_pair);
	}
	if (PHASYNC_G(orig_popen) && (f = phasync_find_ifunc("popen", sizeof("popen") - 1))) {
		f->handler = PHASYNC_G(orig_popen);
	}
	if (PHASYNC_G(orig_exec) && (f = phasync_find_ifunc("exec", sizeof("exec") - 1))) {
		f->handler = PHASYNC_G(orig_exec);
	}
	if (PHASYNC_G(orig_system) && (f = phasync_find_ifunc("system", sizeof("system") - 1))) {
		f->handler = PHASYNC_G(orig_system);
	}
	if (PHASYNC_G(orig_passthru) && (f = phasync_find_ifunc("passthru", sizeof("passthru") - 1))) {
		f->handler = PHASYNC_G(orig_passthru);
	}
	if (PHASYNC_G(orig_shell_exec) && (f = phasync_find_ifunc("shell_exec", sizeof("shell_exec") - 1))) {
		f->handler = PHASYNC_G(orig_shell_exec);
	}
	if (PHASYNC_G(orig_proc_close) && (f = phasync_find_ifunc("proc_close", sizeof("proc_close") - 1))) {
		f->handler = PHASYNC_G(orig_proc_close);
	}
	if (PHASYNC_G(orig_curl_exec) && (f = phasync_find_ifunc("curl_exec", sizeof("curl_exec") - 1))) {
		f->handler = PHASYNC_G(orig_curl_exec);
	}
	if (PHASYNC_G(orig_sem_acquire) && (f = phasync_find_ifunc("sem_acquire", sizeof("sem_acquire") - 1))) {
		f->handler = PHASYNC_G(orig_sem_acquire);
	}
	if (PHASYNC_G(orig_msg_receive) && (f = phasync_find_ifunc("msg_receive", sizeof("msg_receive") - 1))) {
		f->handler = PHASYNC_G(orig_msg_receive);
	}
	if (PHASYNC_G(orig_curl_multi_select) && (f = phasync_find_ifunc("curl_multi_select", sizeof("curl_multi_select") - 1))) {
		f->handler = PHASYNC_G(orig_curl_multi_select);
	}
	if (PHASYNC_G(orig_socket_connect) && (f = phasync_find_ifunc("socket_connect", sizeof("socket_connect") - 1))) {
		f->handler = PHASYNC_G(orig_socket_connect);
	}
	if (PHASYNC_G(orig_socket_close) && (f = phasync_find_ifunc("socket_close", sizeof("socket_close") - 1))) {
		f->handler = PHASYNC_G(orig_socket_close);
	}
	for (size_t i = 0; i < PHASYNC_SOCK_NFUNCS; i++) {
		if (PHASYNC_G(orig_sock)[i] && (f = phasync_find_ifunc(phasync_sock_funcs[i].name, strlen(phasync_sock_funcs[i].name)))) {
			f->handler = PHASYNC_G(orig_sock)[i];
		}
	}
	zend_hash_clean(&PHASYNC_G(sock_hooks));
	if (PHASYNC_G(orig_pcntl_waitpid) && (f = phasync_find_ifunc("pcntl_waitpid", sizeof("pcntl_waitpid") - 1))) {
		f->handler = PHASYNC_G(orig_pcntl_waitpid);
	}
	if (PHASYNC_G(orig_pcntl_wait) && (f = phasync_find_ifunc("pcntl_wait", sizeof("pcntl_wait") - 1))) {
		f->handler = PHASYNC_G(orig_pcntl_wait);
	}
	for (size_t i = 0; i < PHASYNC_FS_NFUNCS; i++) {
		if (PHASYNC_G(orig_fs)[i] && (f = phasync_find_ifunc(phasync_fs_funcs[i].name, strlen(phasync_fs_funcs[i].name)))) {
			f->handler = PHASYNC_G(orig_fs)[i];
		}
	}
	zend_hash_clean(&PHASYNC_G(fs_hooks));
	if (PHASYNC_G(orig_stream_select) && (f = phasync_find_ifunc("stream_select", sizeof("stream_select") - 1))) {
		f->handler = PHASYNC_G(orig_stream_select);
	}
	if (PHASYNC_G(orig_socket_select) && (f = phasync_find_ifunc("socket_select", sizeof("socket_select") - 1))) {
		f->handler = PHASYNC_G(orig_socket_select);
	}
	if (PHASYNC_G(orig_sleep) && (f = phasync_find_ifunc("sleep", sizeof("sleep") - 1))) {
		f->handler = PHASYNC_G(orig_sleep);
	}
	if (PHASYNC_G(orig_header_register_callback) && (f = phasync_find_ifunc("header_register_callback", sizeof("header_register_callback") - 1))) {
		f->handler = PHASYNC_G(orig_header_register_callback);
	}
	if (PHASYNC_G(orig_usleep) && (f = phasync_find_ifunc("usleep", sizeof("usleep") - 1))) {
		f->handler = PHASYNC_G(orig_usleep);
	}
	if (PHASYNC_G(orig_time_nanosleep) && (f = phasync_find_ifunc("time_nanosleep", sizeof("time_nanosleep") - 1))) {
		f->handler = PHASYNC_G(orig_time_nanosleep);
	}
	if (PHASYNC_G(orig_time_sleep_until) && (f = phasync_find_ifunc("time_sleep_until", sizeof("time_sleep_until") - 1))) {
		f->handler = PHASYNC_G(orig_time_sleep_until);
	}
	if (PHASYNC_G(orig_gethostbyname) && (f = phasync_find_ifunc("gethostbyname", sizeof("gethostbyname") - 1))) {
		f->handler = PHASYNC_G(orig_gethostbyname);
	}
	if (PHASYNC_G(orig_gethostbynamel) && (f = phasync_find_ifunc("gethostbynamel", sizeof("gethostbynamel") - 1))) {
		f->handler = PHASYNC_G(orig_gethostbynamel);
	}
	if (PHASYNC_G(orig_gethostbyaddr) && (f = phasync_find_ifunc("gethostbyaddr", sizeof("gethostbyaddr") - 1))) {
		f->handler = PHASYNC_G(orig_gethostbyaddr);
	}
#ifdef HAVE_FULL_DNS_FUNCS
	{
		static const char *names[] = { "dns_check_record", "checkdnsrr", "dns_get_record", "dns_get_mx", "getmxrr" };
		for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
			if ((f = phasync_find_ifunc(names[k], strlen(names[k])))) {
				if (k < 2 && PHASYNC_G(orig_dns_check_record)) f->handler = PHASYNC_G(orig_dns_check_record);
				else if (k == 2 && PHASYNC_G(orig_dns_get_record)) f->handler = PHASYNC_G(orig_dns_get_record);
				else if (k > 2 && PHASYNC_G(orig_dns_get_mx)) f->handler = PHASYNC_G(orig_dns_get_mx);
			}
		}
	}
#endif
	if (PHASYNC_G(orig_fopen) && (f = phasync_find_ifunc("fopen", sizeof("fopen") - 1))) {
		f->handler = PHASYNC_G(orig_fopen);
	}
	if (PHASYNC_G(orig_fclose) && (f = phasync_find_ifunc("fclose", sizeof("fclose") - 1))) {
		f->handler = PHASYNC_G(orig_fclose);
	}
	if (PHASYNC_G(orig_pclose) && (f = phasync_find_ifunc("pclose", sizeof("pclose") - 1))) {
		f->handler = PHASYNC_G(orig_pclose);
	}
	phasync_conn_hooks_restore();
	PHASYNC_G(hooks_installed) = 0;
}

#ifdef PHASYNC_HAVE_SESSION
/* The files save handler locks the session file with flock(2) in C, which would
 * block the worker while another request of the same worker holds the session:
 * a deadlock, as that request can't run to release it. PS(mod) gets a copy of
 * the handler whose read first waits, cooperatively, until the file can be
 * locked; nothing else runs on PHP's thread between that check and the real
 * lock, so no request of this worker can take it in between. A holder in another
 * process still makes the real flock() wait, as under php-fpm. */
static ps_module phasync_ps_files;
static const ps_module *phasync_ps_files_orig;
static php_ps_globals *phasync_ps;   /* ext/session's globals, NULL if it isn't loaded */
static ZEND_INI_MH((*phasync_ps_save_handler_orig));

/* mod_files' path for a session id (ps_files_path_create()), from session.save_path
 * ("[depth;[mode;]]dir"). */
static bool phasync_ps_files_path(const zend_string *key, char *buf, size_t buflen)
{
# if PHP_VERSION_ID >= 80500
	const char *save_path = phasync_ps->save_path ? ZSTR_VAL(phasync_ps->save_path) : "";
# else
	const char *save_path = phasync_ps->save_path ? phasync_ps->save_path : "";
# endif
	const char *dir = strrchr(save_path, ';');
	size_t depth = dir ? (size_t) ZEND_STRTOL(save_path, NULL, 10) : 0;
	size_t n;

	dir = dir ? dir + 1 : save_path;
	if (*dir == '\0') {
		dir = php_get_temporary_directory();
	}
	n = strlen(dir);
	if (ZSTR_LEN(key) <= depth || buflen < n + 2 * depth + ZSTR_LEN(key) + sizeof("/sess_")) {
		return false;
	}
	memcpy(buf, dir, n);
	buf[n++] = '/';
	for (size_t i = 0; i < depth; i++) {
		buf[n++] = ZSTR_VAL(key)[i];
		buf[n++] = '/';
	}
	memcpy(buf + n, "sess_", 5);
	n += 5;
	memcpy(buf + n, ZSTR_VAL(key), ZSTR_LEN(key) + 1);
	return true;
}

static zend_result phasync_ps_files_read(PS_READ_ARGS)
{
	zval *sleep = phasync_sleep_handler();
	char path[MAXPATHLEN];
	zend_long backoff = 1000;

	if (sleep && EG(active_fiber) && key && phasync_ps_files_path(key, path, sizeof(path))) {
		for (;;) {
			int fd = open(path, O_RDONLY | O_CLOEXEC), rc;
			if (fd < 0) {
				break;                    /* no file yet: nobody holds it */
			}
			rc = flock(fd, LOCK_EX | LOCK_NB);
			close(fd);                    /* releases the probe's lock */
			if (rc == 0 || errno != EWOULDBLOCK) {
				break;
			}
			if (phasync_call_sleep(sleep, backoff) < 0) {
				return FAILURE;           /* cancelled: the exception propagates */
			}
			backoff = MIN(backoff * 2, 50000);
		}
	}
	return phasync_ps_files_orig->s_read(mod_data, key, val, maxlifetime);
}

/* PS(mod), when it is the files handler, becomes the copy above: after
 * ext/session's RINIT set it, and whenever session.save_handler changes. */
static void phasync_ps_files_wrap(void)
{
	if (!phasync_ps->mod || phasync_ps->mod == &phasync_ps_files || strcmp(phasync_ps->mod->s_name, "files") != 0) {
		return;
	}
	if (!phasync_ps_files_orig) {
		phasync_ps_files_orig = phasync_ps->mod;
		phasync_ps_files = *phasync_ps->mod;
		phasync_ps_files.s_read = phasync_ps_files_read;
	}
	if (phasync_ps->mod == phasync_ps_files_orig) {
		phasync_ps->mod = &phasync_ps_files;
	}
}

static ZEND_INI_MH(phasync_ps_save_handler_modify)
{
	zend_result r = phasync_ps_save_handler_orig(entry, new_value, mh_arg1, mh_arg2, mh_arg3, stage);

	if (r == SUCCESS) {
		phasync_ps_files_wrap();
	}
	return r;
}

/* MINIT: find ext/session (NULL: not loaded) and follow session.save_handler. */
static void phasync_ps_files_startup(void)
{
	zend_module_entry *m = zend_hash_str_find_ptr(&module_registry, ZEND_STRL("session"));
	zend_ini_entry *e = zend_hash_str_find_ptr(EG(ini_directives), ZEND_STRL("session.save_handler"));

	if (!m || !e) {
		return;
	}
#ifdef ZTS
	phasync_ps = TSRMG_BULK(*m->globals_id_ptr, php_ps_globals *);
#else
	phasync_ps = m->globals_ptr;
#endif
	phasync_ps_save_handler_orig = e->on_modify;
	e->on_modify = phasync_ps_save_handler_modify;
}

static void phasync_ps_files_shutdown(void)
{
	zend_ini_entry *e;

	if (phasync_ps && (e = zend_hash_str_find_ptr(EG(ini_directives), ZEND_STRL("session.save_handler")))) {
		e->on_modify = phasync_ps_save_handler_orig;
	}
}
#endif

