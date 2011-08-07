/*
 * mod_mysql_vhost - load config from mysql
 *
 * Description:
 *     load config from mysql database
 *
 * Setups:
 *     none
 * Options:
 *     none
 * Actions:
 *     mysql_vhost <connection-options>, <lookup-options>, <default action>;
 *        connection-options:
 *            - host
 *            - user
 *            - passwd
 *            - db
 *            - port
 *            - socket
 *        lookup options:
 *            - hit-ttl: how long to remember a hit (default: 300 [seconds])
 *            - miss-ttl: how long to remember a miss (default: 10 [seconds])
 *            - query: sql query string, pattern (mandatory)
 *            - key: lookup key for hash-table, pattern (default: "%{req.host}")
 *        default action: action to execute on a miss (optional)
 *
 * Example config:
 *     mysql_vhost ([ "db": "sites" ], [ "query": ("SELECT config FROM site WHERE hostname=?", "%{req.host}") ]);
 *
 * Author:
 *     Copyright (c) 2010 Stefan Bühler
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

#include <mysql.h>

#define LOOKUP_MAX_PREPARE_PARAMETERS 10

LI_API gboolean mod_mysql_vhost_init(liModules *mods, liModule *mod);
LI_API gboolean mod_mysql_vhost_free(liModules *mods, liModule *mod);

typedef struct mysql_connect_options mysql_connect_options;
struct mysql_connect_options {
	GString *host, *user, *passwd, *db, *socket;
	guint16 port;
};

typedef struct lookup_options lookup_options;
struct lookup_options {
	gint64 hit_ttl, miss_ttl;

	GString *query;
	GPtrArray *query_patterns;

	liPattern *key;
};

typedef struct mysql_ctx mysql_ctx;
struct mysql_ctx {
	int refcount;

	GMutex *lock;

	MYSQL *mysql;
	MYSQL_STMT *stmt;

	mysql_connect_options connect_options;
	lookup_options lookup_options;
	liAction *act_miss;

	GHashTable *vhosts; /* GString -> vhost */

	liPlugin *p;
};

typedef struct vhost vhost;
struct vhost {
	GString *key;

	ev_tstamp loaded;
	liAction *action;

	GPtrArray *wakeup_jobrefs;
};

typedef struct lookup_tasklet lookup_tasklet;
struct lookup_tasklet {
	mysql_ctx *ctx;

	GPtrArray *query_binds;
};

/* mysql connect option names */
static const GString
	mcon_host = { CONST_STR_LEN("host"), 0 },
	mcon_user = { CONST_STR_LEN("user"), 0 },
	mcon_passwd = { CONST_STR_LEN("passwd"), 0 },
	mcon_db = { CONST_STR_LEN("db"), 0 },
	mcon_port = { CONST_STR_LEN("port"), 0 },
	mcon_socket = { CONST_STR_LEN("socket"), 0 }
;

/* lookup option names */
static const GString
	lon_hit_ttl = { CONST_STR_LEN("hit-ttl"), 0 },
	lon_miss_ttl = { CONST_STR_LEN("miss-ttl"), 0 },
	lon_query = { CONST_STR_LEN("query"), 0 },
	lon_key = { CONST_STR_LEN("key"), 0 }
;

static void vhost_free(liServer *srv, vhost *h) {
	g_string_free(h->key, TRUE);
	li_action_release(srv, h->action);

	if (h->wakeup_jobrefs) {
		guint i;
		for (i = 0; i < h->wakeup_jobrefs->len; i++) {
			liJobRef *ref = g_ptr_array_index(h->wakeup_jobrefs, i);
			li_job_async(ref);
			li_job_ref_release(ref);
		}
		g_ptr_array_free(h->wakeup_jobrefs, TRUE);
		h->wakeup_jobrefs = NULL;
	}

	g_slice_free(vhost, h);
}

static void mysql_ctx_acquire(mysql_ctx* ctx) {
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	g_atomic_int_inc(&ctx->refcount);
}

static void mysql_ctx_release(liServer *srv, gpointer param) {
	mysql_ctx *ctx = param;

	if (NULL == ctx) return;

	assert(g_atomic_int_get(&ctx->refcount) > 0);
	if (!g_atomic_int_dec_and_test(&ctx->refcount)) return;

	g_mutex_free(ctx->lock);

	if (NULL != ctx->connect_options.host) g_string_free(ctx->connect_options.host, TRUE);
	if (NULL != ctx->connect_options.user) g_string_free(ctx->connect_options.user, TRUE);
	if (NULL != ctx->connect_options.passwd) g_string_free(ctx->connect_options.passwd, TRUE);
	if (NULL != ctx->connect_options.db) g_string_free(ctx->connect_options.db, TRUE);
	if (NULL != ctx->connect_options.socket) g_string_free(ctx->connect_options.socket, TRUE);

	if (NULL != ctx->lookup_options.query) g_string_free(ctx->lookup_options.query, TRUE);
	if (NULL != ctx->lookup_options.key) li_pattern_free(ctx->lookup_options.key);
	if (NULL != ctx->lookup_options.query_patterns) {
		guint i;
		for (i = 0; i < ctx->lookup_options.query_patterns->len; i++) {
			li_pattern_free(g_ptr_array_index(ctx->lookup_options.query_patterns, i));
		}
		g_ptr_array_free(ctx->lookup_options.query_patterns, TRUE);
	}

	if (ctx->vhosts) {
		GHashTableIter it;
		gpointer key, value;

		g_hash_table_iter_init(&it, ctx->vhosts);
		while (g_hash_table_iter_next(&it, &key, &value)) {
			vhost *h = value;
			g_hash_table_iter_remove(&it);
			vhost_free(srv, h);
		}
		g_hash_table_destroy(ctx->vhosts);
	}

	li_action_release(srv, ctx->act_miss);

	g_slice_free(mysql_ctx, ctx);
}

static mysql_ctx* mysql_ctx_parse(liServer *srv, liPlugin *p, liValue *config) {
	mysql_ctx *ctx;
	liValue *connect_value, *lookup_value, *miss_value;
	GHashTable *ht;
	GHashTableIter it;
	gpointer pkey, pvalue;

	if (!config || config->type != LI_VALUE_LIST || (config->data.list->len < 2) || (config->data.list->len > 3)) {
		ERROR(srv, "%s", "mysql_vhost: bogus parameters");
		return NULL;
	}

	connect_value = g_array_index(config->data.list, liValue*, 0);
	lookup_value = g_array_index(config->data.list, liValue*, 1);
	miss_value = config->data.list->len >= 3 ? g_array_index(config->data.list, liValue*, 2) : NULL;

	if (connect_value->type != LI_VALUE_HASH) {
		ERROR(srv, "%s", "mysql_vhost: expect a hash of connection options as first parameter");
		return NULL;
	}

	if (lookup_value->type != LI_VALUE_HASH) {
		ERROR(srv, "%s", "mysql_vhost: expect a hash of lookup options as second parameter");
		return NULL;
	}

	if (miss_value && miss_value->type != LI_VALUE_ACTION) {
		ERROR(srv, "%s", "mysql_vhost: expect a fallback action as third parameter");
		return NULL;
	}

	ctx = g_slice_new0(mysql_ctx);
	ctx->refcount = 1;
	ctx->p = p;
	ctx->lock = g_mutex_new();
	ctx->vhosts = g_hash_table_new((GHashFunc) g_string_hash, (GEqualFunc) g_string_equal);

	ht = connect_value->data.hash;
	g_hash_table_iter_init(&it, ht);
	while (g_hash_table_iter_next(&it, &pkey, &pvalue)) {
		GString *key = pkey;
		liValue *value = pvalue;

		if (g_string_equal(key, &mcon_host)) {
			if (value->type != LI_VALUE_STRING) {
				ERROR(srv, "mysql_vhost connect option '%s' expects string as parameter", mcon_host.str);
				goto option_failed;
			}
			ctx->connect_options.host = li_value_extract_string(value);
		} else if (g_string_equal(key, &mcon_user)) {
			if (value->type != LI_VALUE_STRING) {
				ERROR(srv, "mysql_vhost connect option '%s' expects string as parameter", mcon_user.str);
				goto option_failed;
			}
			ctx->connect_options.user = li_value_extract_string(value);
		} else if (g_string_equal(key, &mcon_passwd)) {
			if (value->type != LI_VALUE_STRING) {
				ERROR(srv, "mysql_vhost connect option '%s' expects string as parameter", mcon_passwd.str);
				goto option_failed;
			}
			ctx->connect_options.passwd = li_value_extract_string(value);
		} else if (g_string_equal(key, &mcon_db)) {
			if (value->type != LI_VALUE_STRING) {
				ERROR(srv, "mysql_vhost connect option '%s' expects string as parameter", mcon_db.str);
				goto option_failed;
			}
			ctx->connect_options.db = li_value_extract_string(value);
		} else if (g_string_equal(key, &mcon_port)) {
			if (value->type != LI_VALUE_NUMBER || value->data.number <= 0 || value->data.number > 65535) {
				ERROR(srv, "mysql_vhost connect option '%s' expects positive 16bit-integer (0-65535) as parameter", mcon_port.str);
				goto option_failed;
			}
			ctx->connect_options.port = value->data.number;
		} else if (g_string_equal(key, &mcon_socket)) {
			if (value->type != LI_VALUE_STRING) {
				ERROR(srv, "mysql_vhost connect option '%s' expects string as parameter", mcon_user.str);
				goto option_failed;
			}
			ctx->connect_options.socket = li_value_extract_string(value);
		} else {
			ERROR(srv, "unknown mysql_vhost connect option '%s'", key->str);
			goto option_failed;
		}
	}

	ht = lookup_value->data.hash;
	g_hash_table_iter_init(&it, ht);
	while (g_hash_table_iter_next(&it, &pkey, &pvalue)) {
		GString *key = pkey;
		liValue *value = pvalue;

		if (g_string_equal(key, &lon_hit_ttl)) {
			if (value->type != LI_VALUE_NUMBER) {
				ERROR(srv, "mysql_vhost lookup option '%s' expects number as parameter", lon_hit_ttl.str);
				goto option_failed;
			}
			ctx->lookup_options.hit_ttl = value->data.number;
		} else if (g_string_equal(key, &lon_miss_ttl)) {
			if (value->type != LI_VALUE_NUMBER) {
				ERROR(srv, "mysql_vhost lookup option '%s' expects number as parameter", lon_miss_ttl.str);
				goto option_failed;
			}
			ctx->lookup_options.miss_ttl = value->data.number;
		} else if (g_string_equal(key, &lon_query)) {
			guint i;
			GArray *list;
			if (value->type != LI_VALUE_LIST || value->data.list->len == 0) {
				ERROR(srv, "mysql_vhost lookup option '%s' expects non-empty list of strings as parameter", lon_query.str);
				goto option_failed;
			}
			list = value->data.list;
			if (list->len > LOOKUP_MAX_PREPARE_PARAMETERS + 1) {
				ERROR(srv, "mysql_vhost lookup option '%s': too many prepare parameters", lon_query.str);
				goto option_failed;
			}
			for (i = 0; i < list->len; i++) if (g_array_index(list, liValue*, i)->type != LI_VALUE_STRING) {
				ERROR(srv, "mysql_vhost lookup option '%s': expected list of strings", lon_query.str);
				goto option_failed;
			}
			/* first entry is the query string */
			ctx->lookup_options.query = li_value_extract_string(g_array_index(list, liValue*, 0));
			/* now come the patterns */
			ctx->lookup_options.query_patterns = g_ptr_array_new();
			for (i = 1; i < list->len; i++) {
				const gchar *s = g_array_index(list, liValue*, i)->data.string->str;
				liPattern *pattern = li_pattern_new(srv, s);
				if (NULL == pattern) {
					ERROR(srv, "mysql_vhost: couldn't parse pattern for lookup query parameter '%s'", s);
					goto option_failed;
				}
				g_ptr_array_add(ctx->lookup_options.query_patterns, pattern);
			}
		} else if (g_string_equal(key, &lon_key)) {
			if (value->type != LI_VALUE_STRING) {
				ERROR(srv, "mysql_vhost lookup option '%s' expects string as parameter", lon_key.str);
				goto option_failed;
			}
			ctx->lookup_options.key = li_pattern_new(srv, value->data.string->str);
			if (NULL == ctx->lookup_options.key) {
				ERROR(srv, "mysql_vhost: couldn't parse pattern for lookup key '%s'", value->data.string->str);
				goto option_failed;
			}
		} else {
			ERROR(srv, "unknown mysql_vhost lookup option '%s'", key->str);
			goto option_failed;
		}
	}

	ctx->act_miss = li_value_extract_action(miss_value);

	return ctx;

option_failed:
	mysql_ctx_release(srv, ctx);
	return NULL;
}
#if 0
static GString* mc_ctx_build_key(mysql_vhost_ctx *ctx, liVRequest *vr) {
	GMatchInfo *match_info = NULL;
	GString *key = g_string_sized_new(255);

	g_string_truncate(key, 0);

	if (vr->action_stack.regex_stack->len) {
		GArray *rs = vr->action_stack.regex_stack;
		match_info = g_array_index(rs, liActionRegexStackElement, rs->len - 1).match_info;
	}

	li_pattern_eval(vr, key, ctx->pattern, NULL, NULL, li_pattern_regex_cb, match_info);

	li_mysql_vhost_mutate_key(key);

	return key;
}

static liMemcachedCon* mc_ctx_prepare(mysql_vhost_ctx *ctx, liWorker *wrk) {
	liMemcachedCon *con = ctx->worker_client_ctx[wrk->ndx];

	if (!con) {
		con = li_mysql_vhost_con_new(wrk->loop, ctx->addr);
		ctx->worker_client_ctx[wrk->ndx] = con;
	}

	return con;
}

static void memcache_callback(liMemcachedRequest *request, liMemcachedResult result, liMemcachedItem *item, GError **err) {
	memcache_request *req = request->cb_data;
	liVRequest *vr = req->vr;

	/* request done */
	req->req = NULL;

	if (!vr) {
		g_slice_free(memcache_request, req);
		return;
	}

	switch (result) {
	case LI_MEMCACHED_OK: /* STORED, VALUE, DELETED */
		/* steal buffer */
		req->buffer = item->data;
		item->data = NULL;
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "mysql_vhost.lookup: key '%s' found, flags = %u", item->key->str, (guint) item->flags);
		}
		break;
	case LI_MEMCACHED_NOT_FOUND:
		/* ok, nothing to do - we just didn't find an entry */
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "mysql_vhost.lookup: key not found");
		}
		break;
	case LI_MEMCACHED_NOT_STORED:
	case LI_MEMCACHED_EXISTS:
		VR_ERROR(vr, "mysql_vhost error: %s", "unexpected result");
		/* TODO (not possible for lookup) */
		break;
	case LI_MEMCACHED_RESULT_ERROR:
		if (err && *err) {
			if (LI_MEMCACHED_DISABLED != (*err)->code) {
				VR_ERROR(vr, "mysql_vhost error: %s", (*err)->message);
			}
		} else {
			VR_ERROR(vr, "mysql_vhost error: %s", "Unknown error");
		}
		break;
	}

	li_vrequest_joblist_append(vr);
}

static liHandlerResult mc_handle_lookup(liVRequest *vr, gpointer param, gpointer *context) {
	mysql_vhost_ctx *ctx = param;
	memcache_request *req = *context;

	if (req) {
		static const GString default_mime_str = { CONST_STR_LEN("application/octet-stream"), 0 };

		liBuffer *buf = req->buffer;
		const GString *mime_str;

		if (NULL != req->req) return LI_HANDLER_WAIT_FOR_EVENT; /* not done yet */

		g_slice_free(memcache_request, req);
		*context = NULL;

		if (NULL == buf) {
			/* miss */
			if (ctx->act_miss) li_action_enter(vr, ctx->act_miss);
			return LI_HANDLER_GO_ON;
		}

		if (!li_vrequest_handle_direct(vr)) {
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "mysql_vhost.lookup: request already handled");
			}
			li_buffer_release(buf);
			return LI_HANDLER_GO_ON;
		}

		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "mysql_vhost.lookup: key found, handling request");
		}

		li_chunkqueue_append_buffer(vr->out, buf);

		vr->response.http_status = 200;

		mime_str = li_mimetype_get(vr, vr->request.uri.path);
		if (!mime_str) mime_str = &default_mime_str;
		li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), GSTR_LEN(mime_str));

		/* hit */
		if (ctx->act_found) li_action_enter(vr, ctx->act_found);
		return LI_HANDLER_GO_ON;
	} else {
		liMemcachedCon *con;
		GString *key;
		GError *err = NULL;

		if (li_vrequest_is_handled(vr)) {
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "mysql_vhost.lookup: request already handled");
			}
			return LI_HANDLER_GO_ON;
		}

		con = mc_ctx_prepare(ctx, vr->wrk);
		key = mc_ctx_build_key(ctx, vr);

		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "mysql_vhost.lookup: looking up key '%s'", key->str);
		}

		req = g_slice_new0(memcache_request);
		req->req = li_mysql_vhost_get(con, key, memcache_callback, req, &err);
		g_string_free(key, TRUE);

		if (NULL == req->req) {
			if (NULL != err) {
				if (LI_MEMCACHED_DISABLED != err->code) {
					VR_ERROR(vr, "mysql_vhost.lookup: get failed: %s", err->message);
				}
				g_clear_error(&err);
			} else {
				VR_ERROR(vr, "mysql_vhost.lookup: get failed: %s", "Unkown error");
			}
			g_slice_free(memcache_request, req);

			/* miss */
			if (ctx->act_miss) li_action_enter(vr, ctx->act_miss);

			return LI_HANDLER_GO_ON;
		}
		req->vr = vr;

		*context = req;

		return LI_HANDLER_WAIT_FOR_EVENT;
	}
}

static liHandlerResult mc_lookup_handle_free(liVRequest *vr, gpointer param, gpointer context) {
	memcache_request *req = context;
	UNUSED(vr);
	UNUSED(param);

	if (NULL == req->req) {
		li_buffer_release(req->buffer);
		g_slice_free(memcache_request, req);
	} else {
		req->vr = NULL;
	}

	return LI_HANDLER_GO_ON;
}

static void memcache_store_filter_free(liVRequest *vr, liFilter *f) {
	memcache_filter *mf = (memcache_filter*) f->param;
	UNUSED(vr);

	mc_ctx_release(vr->wrk->srv, mf->ctx);
	li_buffer_release(mf->buf);

	g_slice_free(memcache_filter, mf);
}

static liHandlerResult memcache_store_filter(liVRequest *vr, liFilter *f) {
	memcache_filter *mf = (memcache_filter*) f->param;

	if (f->in->is_closed && 0 == f->in->length && f->out->is_closed) {
		/* nothing to do anymore */
		return LI_HANDLER_GO_ON;
	}

	if (f->out->is_closed) {
		li_chunkqueue_skip_all(f->in);
		f->in->is_closed = TRUE;
		return LI_HANDLER_GO_ON;
	}

	/* if already in "forward" mode */
	if (NULL == mf->buf) goto forward;

	/* check if size still fits into buffer */
	if ((gssize) (f->in->length + mf->buf->used) > (gssize) mf->ctx->maxsize) {
		/* response too big, switch to "forward" mode */
		li_buffer_release(mf->buf);
		mf->buf = NULL;
		goto forward;
	}

	while (0 < f->in->length) {
		char *data;
		off_t len;
		liChunkIter ci;
		liHandlerResult res;

		if (0 == f->in->length) break;

		ci = li_chunkqueue_iter(f->in);

		if (LI_HANDLER_GO_ON != (res = li_chunkiter_read(vr, ci, 0, 16*1024, &data, &len)))
			return res;

		if ((gssize) (len + mf->buf->used) > (gssize) mf->ctx->maxsize) {
			/* response too big, switch to "forward" mode */
			li_buffer_release(mf->buf);
			mf->buf = NULL;
			goto forward;
		}

		memcpy(mf->buf->addr + mf->buf->used, data, len);
		mf->buf->used += len;

		li_chunkqueue_steal_len(f->out, f->in, len);
	}

	if (f->in->is_closed) {
		/* finally: store response in mysql_vhost */

		liMemcachedCon *con;
		GString *key;
		GError *err = NULL;
		liMemcachedRequest *req;
		mysql_vhost_ctx *ctx = mf->ctx;

		f->out->is_closed = TRUE;

		con = mc_ctx_prepare(ctx, vr->wrk);
		key = mc_ctx_build_key(ctx, vr);

		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "mysql_vhost.store: storing response for key '%s'", key->str);
		}

		req = li_mysql_vhost_set(con, key, ctx->flags, ctx->ttl, mf->buf, NULL, NULL, &err);
		g_string_free(key, TRUE);
		li_buffer_release(mf->buf);
		mf->buf = NULL;

		if (NULL == req) {
			if (NULL != err) {
				if (LI_MEMCACHED_DISABLED != err->code) {
					VR_ERROR(vr, "mysql_vhost.store: set failed: %s", err->message);
				}
				g_clear_error(&err);
			} else {
				VR_ERROR(vr, "mysql_vhost.store: set failed: %s", "Unkown error");
			}
		}
	}

	return LI_HANDLER_GO_ON;

forward:
	li_chunkqueue_steal_all(f->out, f->in);
	if (f->in->is_closed) f->out->is_closed = f->in->is_closed;
	return LI_HANDLER_GO_ON;
}

static liHandlerResult mc_handle_store(liVRequest *vr, gpointer param, gpointer *context) {
	mysql_vhost_ctx *ctx = param;
	memcache_filter *mf;
	UNUSED(context);

	VREQUEST_WAIT_FOR_RESPONSE_HEADERS(vr);

	if (vr->response.http_status != 200) return LI_HANDLER_GO_ON;

	mf = g_slice_new0(memcache_filter);
	mf->ctx = ctx;
	mc_ctx_acquire(ctx);
	mf->buf = li_buffer_new(ctx->maxsize);

	li_vrequest_add_filter_out(vr, memcache_store_filter, memcache_store_filter_free, mf);

	return LI_HANDLER_GO_ON;
}

static liAction* mc_lookup_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	mysql_vhost_ctx *ctx;
	liValue *config = val, *act_found = NULL, *act_miss = NULL;
	UNUSED(wrk);
	UNUSED(userdata);

	if (val && LI_VALUE_LIST == val->type) {
		GArray *list = val->data.list;
		config = NULL;

		if (list->len > 3) {
			ERROR(srv, "%s", "mysql_vhost.lookup: too many arguments");
			return NULL;
		}

		if (list->len >= 1) config = g_array_index(list, liValue*, 0);
		if (list->len >= 2) act_found = g_array_index(list, liValue*, 1);
		if (list->len >= 3) act_miss = g_array_index(list, liValue*, 2);

		if (config && config->type != LI_VALUE_HASH) {
			ERROR(srv, "%s", "mysql_vhost.lookup: expected hash as first argument");
			return NULL;
		}

		if (act_found && act_found->type != LI_VALUE_ACTION) {
			ERROR(srv, "%s", "mysql_vhost.lookup: expected action as second argument");
			return NULL;
		}

		if (act_miss && act_miss->type != LI_VALUE_ACTION) {
			ERROR(srv, "%s", "mysql_vhost.lookup: expected action as third argument");
			return NULL;
		}
	}

	ctx = mc_ctx_parse(srv, p, config);

	if (!ctx) return NULL;

	if (act_found) ctx->act_found = li_value_extract_action(act_found);
	if (act_miss) ctx->act_miss = li_value_extract_action(act_miss);

	return li_action_new_function(mc_handle_lookup, mc_lookup_handle_free, mc_ctx_release, ctx);
}

static liAction* mc_store_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	mysql_vhost_ctx *ctx;
	UNUSED(wrk);
	UNUSED(userdata);

	ctx = mc_ctx_parse(srv, p, val);

	if (!ctx) return NULL;

	return li_action_new_function(mc_handle_store, NULL, mc_ctx_release, ctx);
}
#endif

static const liPluginOption options[] = {
	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
// 	{ "mysql_vhost.lookup", mc_lookup_create, NULL },
// 	{ "mysql_vhost.store", mc_store_create, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};


static void mysql_vhost_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}

gboolean mod_mysql_vhost_init(liModules *mods, liModule *mod) {
	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_mysql_vhost", mysql_vhost_init, NULL);

	return mod->config != NULL;
}

gboolean mod_mysql_vhost_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}
