/*
 * mod_trigger_b4_dl - trigger before download
 *
 * Description:
 *
 * Setups:
 *     none
 * Options:
 *     none
 * Actions:
 *     trigger_b4_dl [options]
 *         options:
 *             "sort" => criterium             - string, one of "name", "size" or "type"
 *             "css" => url                    - string, external css to use for styling, default: use internal css
 *
 *             "hide-dotfiles" => bool         - hide entries beginning with a dot, default: true
 *             "hide-tildefiles" => bool       - hide entries ending with a tilde (~), often used for backups, default: true
 *             "hide-directories" => bool      - hide directories from the directory listing, default: false
 *
 *             "include-header" => bool        - include HEADER.txt above the directory listing, default: false
 *             "hide-header" => bool           - hide HEADER.txt from the directory listing, default: false
 *             "encode-header" => bool         - html-encode HEADER.txt (if included), set to false if it contains real HTML, default: true
 *
 *             "include-readme" => bool        - include README.txt below the directory listing, default: true
 *             "hide-readme" => bool           - hide README.txt from the directory listing, default: false
 *             "encode-readme" => bool         - html-encode README.txt (if included), set to false if it contains real HTML, default: true
 *
 *             "exclude-suffix" => suffixlist  - list of strings, filter entries that end with one of the strings supplied
 *             "exclude-prefix" => prefixlist  - list of strings, filter entries that begin with one of the strings supplied
 *
 *             "debug" => bool                 - outout debug information to log, default: false
 *
 * Example config:
 *
 * Todo:
 *
 * Author:
 *     Copyright (c) 2010 Stefan Bühler
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

LI_API gboolean mod_trigger_b4_dl_init(liModules *mods, liModule *mod);
LI_API gboolean mod_trigger_b4_dl_free(liModules *mods, liModule *mod);

#include <lighttpd/memcached.h>

typedef struct plugin_data plugin_data;
struct plugin_data {
	GQueue prepare_ctx;
};

typedef struct mc_config mc_config;
struct mc_config {
	liMemcachedCon **worker_client_ctx;

	liPattern *pattern_key, *pattern_redirect;
	liSocketAddress addr;
	ev_tstamp ttl;

	liPlugin *p;
	GList prepare_link;
};

static void mc_config_free(liServer *srv, gpointer param) {
	mc_config *config = param;
	guint i;

	if (NULL == config) return;

	if (config->worker_client_ctx) {
		for (i = 0; i < srv->worker_count; i++) {
			li_memcached_con_release(config->worker_client_ctx[i]);
		}
		g_slice_free1(sizeof(liMemcachedCon*) * srv->worker_count, config->worker_client_ctx);
	}

	li_sockaddr_clear(&config->addr);

	li_pattern_free(config->pattern_key);
	li_pattern_free(config->pattern_redirect);

	if (config->prepare_link.data) { /* still in LI_SERVER_INIT */
		plugin_data *pd = config->p->data;
		g_queue_unlink(&pd->prepare_ctx, &config->prepare_link);
		config->prepare_link.data = NULL;
	}

	g_slice_free(mc_config, config);
}


static liHandlerResult mc_grant(liVRequest *vr, gpointer param, gpointer *context) {
	mc_config *config = param;
	liMemcachedCon *mc_con;
	GError *err = NULL;
	GString *key = vr->wrk->tmp_str;
	UNUSED(context);

	mc_con = config->worker_client_ctx[vr->wrk->ndx];
	if (NULL == mc_con) {
		mc_con = li_memcached_con_new(vr->wrk->loop, config->addr);
		config->worker_client_ctx[vr->wrk->ndx] = mc_con;
	}

	li_pattern_eval(vr, key, config->pattern_key, NULL, NULL, NULL, NULL);

	if (NULL == li_memcached_set(mc_con, key, 0, config->ttl, NULL, NULL, NULL, &err)) {
		VR_ERROR(vr, "Couldn't store grant: %s", err->message);
		g_error_free(err);
	}

	return LI_HANDLER_GO_ON;
}

typedef struct mc_check_ctx mc_check_ctx;
struct mc_check_ctx {
	liMemcachedRequest *req;
	enum { MC_CHECK_WAIT, MC_CHECK_ALLOW, MC_CHECK_DENY } result;
	liVRequest *vr;
};

static void mc_check_cb(liMemcachedRequest *request, liMemcachedResult result, liMemcachedItem *item, GError **err) {
	mc_check_ctx *ctx = request->cb_data;
	UNUSED(item);

	if (NULL == ctx) return;

	switch (result) {
	case LI_MEMCACHED_OK:
		ctx->result = MC_CHECK_ALLOW;
		break;
	case LI_MEMCACHED_NOT_FOUND:
		ctx->result = MC_CHECK_DENY;
		break;
	default:
		ctx->result = MC_CHECK_DENY;
		break;
	}

	if (NULL != err && NULL != *err) {
		VR_ERROR(ctx->vr, "memcached lookup error: %s", (*err)->message);
	}

	li_vrequest_joblist_append(ctx->vr);
}

static void mc_check_ctx_free(gpointer context) {
	mc_check_ctx *ctx = context;

	if (NULL == ctx) return;

	if (NULL != ctx->req) {
		ctx->req->cb_data = NULL;
	}
	g_slice_free(mc_check_ctx, ctx);
}

static liHandlerResult mc_check(liVRequest *vr, gpointer param, gpointer *context) {
	mc_config *config = param;
	mc_check_ctx *ctx = *context;
	liHandlerResult res = LI_HANDLER_ERROR;

	if (li_vrequest_is_handled(vr)) {
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "skipping trigger_b4_dl.memcache_check as request is already handled with current status %i", vr->response.http_status);
		}
		return LI_HANDLER_GO_ON;
	}

	if (NULL == ctx) {
		GError *err = NULL;
		GString *key = vr->wrk->tmp_str;
		liMemcachedCon *mc_con;

		mc_con = config->worker_client_ctx[vr->wrk->ndx];
		if (NULL == mc_con) {
			mc_con = li_memcached_con_new(vr->wrk->loop, config->addr);
			config->worker_client_ctx[vr->wrk->ndx] = mc_con;
		}

		*context = ctx = g_slice_new0(mc_check_ctx);

		li_pattern_eval(vr, key, config->pattern_key, NULL, NULL, NULL, NULL);

		ctx->result = MC_CHECK_WAIT;
		ctx->req = li_memcached_get(mc_con, key, mc_check_cb, context, &err);
		ctx->vr = vr;

		if (NULL == ctx->req) {
			VR_ERROR(vr, "Couldn't check access: %s", err->message);
			g_error_free(err);
			goto free_context; /* error */
		}
	}

	switch (ctx->result) {
	case MC_CHECK_WAIT:
		if (NULL == ctx->req) goto free_context; /* error */
		return LI_HANDLER_WAIT_FOR_EVENT;
	case MC_CHECK_ALLOW:
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "grant access in trigger_b4_dl.memcache_check");
		}
		res = LI_HANDLER_GO_ON;
		goto free_context;
	case MC_CHECK_DENY:
		if (!li_vrequest_handle_direct(vr))
			goto free_context; /* error */
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "deny access in trigger_b4_dl.memcache_check => redirect");
		}
		{
			GString *dest = vr->wrk->tmp_str;
			li_pattern_eval(vr, dest, config->pattern_redirect, NULL, NULL, NULL, NULL);
			vr->response.http_status = 301;
			li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Location"), GSTR_LEN(dest));
		}
		res = LI_HANDLER_GO_ON;
		break;
	}
	

free_context:
	*context = NULL;
	if (NULL != ctx->req) {
		ctx->req->cb_data = NULL;
	}
	g_slice_free(mc_check_ctx, ctx);

	return res;
}

static const liPluginOption options[] = {
	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};


static void plugin_trigger_b4_dl_prepare(liServer *srv, liPlugin *p) {
	plugin_data *pd = p->data;
	GList *conf_link;
	mc_config *ctx;

	while (NULL != (conf_link = g_queue_pop_head_link(&pd->prepare_ctx))) {
		ctx = conf_link->data;
		ctx->worker_client_ctx = g_slice_alloc0(sizeof(liMemcachedCon*) * srv->worker_count);
		conf_link->data = NULL;
	}
}

static void plugin_trigger_b4_dl_free(liServer *srv, liPlugin *p) {
	plugin_data *pd = p->data;

	UNUSED(srv);

	g_slice_free(plugin_data, pd);
}

static void plugin_trigger_b4_dl_init(liServer *srv, liPlugin *p, gpointer userdata) {
	plugin_data *pd;

	UNUSED(srv); UNUSED(userdata);

	pd = g_slice_new0(plugin_data);
	p->data = pd;

	p->options = options;
	p->actions = actions;
	p->setups = setups;

	p->free = plugin_trigger_b4_dl_free;
	p->handle_prepare = plugin_trigger_b4_dl_prepare;
}


gboolean mod_trigger_b4_dl_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_trigger_b4_dl", plugin_trigger_b4_dl_init, NULL);

	return mod->config != NULL;
}

gboolean mod_trigger_b4_dl_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}
