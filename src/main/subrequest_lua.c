
#include <lighttpd/core_lua.h>
#include <lighttpd/actions_lua.h>

#include <lualib.h>
#include <lauxlib.h>

typedef struct liSubrequest liSubrequest;
struct liSubrequest {
	liWorker *wrk;

	lua_State *L;
	int func_notify_ref, func_error_ref;

	liVRequest *vr;
	liJobRef *parentvr_ref;

	liConInfo coninfo;

	gboolean have_response_headers;
	gboolean notified_out_closed, notified_response_headers;
	goffset notified_out_bytes;
};

static int li_lua_push_subrequest(lua_State *L, liSubrequest *sr);
static liSubrequest* li_lua_get_subrequest(lua_State *L, int ndx);
static int lua_subrequest_gc(lua_State *L);

#define LUA_SUBREQUEST "liSubrequest*"

typedef int (*lua_Subrequest_Attrib)(liSubrequest *sr, lua_State *L);

static int lua_subrequest_attr_read_in(liSubrequest *sr, lua_State *L) {
	if (NULL == sr->vr) { lua_pushnil(L); return 1; }
	li_lua_push_chunkqueue(L, sr->vr->vr_in);
	return 1;
}

static int lua_subrequest_attr_read_out(liSubrequest *sr, lua_State *L) {
	if (NULL == sr->vr) { lua_pushnil(L); return 1; }
	li_lua_push_chunkqueue(L, sr->vr->vr_out);
	return 1;
}

static int lua_subrequest_attr_read_is_done(liSubrequest *sr, lua_State *L) {
	lua_pushboolean(L, (NULL == sr->vr) || sr->vr->vr_out->is_closed);
	return 1;
}

static int lua_subrequest_attr_read_have_headers(liSubrequest *sr, lua_State *L) {
	lua_pushboolean(L, sr->have_response_headers);
	return 1;
}

static int lua_subrequest_attr_read_vr(liSubrequest *sr, lua_State *L) {
	li_lua_push_vrequest(L, sr->vr);
	return 1;
}

#define AR(m) { #m, lua_subrequest_attr_read_##m, NULL }
#define AW(m) { #m, NULL, lua_subrequest_attr_write_##m }
#define ARW(m) { #m, lua_subrequest_attr_read_##m, lua_subrequest_attr_write_##m }

static const struct {
	const char* key;
	lua_Subrequest_Attrib read_attr, write_attr;
} subrequest_attribs[] = {
	AR(in),
	AR(out),
	AR(is_done),
	AR(have_headers),
	AR(vr),

	{ NULL, NULL, NULL }
};

#undef AR
#undef AW
#undef ARW

static int lua_subrequest_index(lua_State *L) {
	liSubrequest *sr;
	const char *key;
	int i;

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	if (li_lua_metatable_index(L)) return 1;

	sr = li_lua_get_subrequest(L, 1);
	if (!sr) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; subrequest_attribs[i].key ; i++) {
		if (0 == strcmp(key, subrequest_attribs[i].key)) {
			if (subrequest_attribs[i].read_attr)
				return subrequest_attribs[i].read_attr(sr, L);
			break;
		}
	}

	lua_pushstring(L, "cannot read attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, " in subrequest");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static int lua_subrequest_newindex(lua_State *L) {
	liSubrequest *sr;
	const char *key;
	int i;

	if (lua_gettop(L) != 3) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	sr = li_lua_get_subrequest(L, 1);
	if (!sr) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; subrequest_attribs[i].key ; i++) {
		if (0 == strcmp(key, subrequest_attribs[i].key)) {
			if (subrequest_attribs[i].write_attr)
				return subrequest_attribs[i].write_attr(sr, L);
			break;
		}
	}

	lua_pushstring(L, "cannot write attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, "in subrequest");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static const luaL_Reg subrequest_mt[] = {
	{ "__index", lua_subrequest_index },
	{ "__newindex", lua_subrequest_newindex },

	{ "__gc", lua_subrequest_gc },

	{ NULL, NULL }
};


static void init_subrequest_mt(lua_State *L) {
	luaL_register(L, NULL, subrequest_mt);
}

void li_lua_init_subrequest_mt(lua_State *L) {
	if (luaL_newmetatable(L, LUA_SUBREQUEST)) {
		init_subrequest_mt(L);
	}
	lua_pop(L, 1);
}

static liSubrequest* li_lua_get_subrequest(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_SUBREQUEST);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(liSubrequest**) lua_touserdata(L, ndx);
}

static int li_lua_push_subrequest(lua_State *L, liSubrequest *sr) {
	liSubrequest **psr;

	psr = (liSubrequest**) lua_newuserdata(L, sizeof(liSubrequest*));
	*psr = sr;

	if (luaL_newmetatable(L, LUA_SUBREQUEST)) {
		init_subrequest_mt(L);
	}

	lua_setmetatable(L, -2);
	return 1;
}

static void subvr_run_lua(liSubrequest *sr, int func_ref) {
	liServer *srv = sr->wrk->srv;
	lua_State *L = sr->L;
	gboolean dolock = (L == srv->L);
	int errfunc;

	if (NULL == L || LUA_REFNIL == func_ref) return;

	if (dolock) li_lua_lock(srv);

	lua_rawgeti(L, LUA_REGISTRYINDEX, func_ref);

	li_lua_push_subrequest(L, sr);

	errfunc = li_lua_push_traceback(L, 1);
	if (lua_pcall(L, 1, 0, errfunc)) {
		ERROR(srv, "lua_pcall(): %s", lua_tostring(L, -1));
		lua_pop(L, 1);
	}
	lua_remove(L, errfunc);

	if (dolock) li_lua_unlock(srv);
}

static void subvr_release_lua(liSubrequest *sr) {
	liServer *srv = sr->wrk->srv;
	lua_State *L = sr->L;
	gboolean dolock = (L == srv->L);

	if (NULL == L) return;

	sr->L = NULL;

	if (dolock) li_lua_lock(srv);

	luaL_unref(L, LUA_REGISTRYINDEX, sr->func_notify_ref);
	luaL_unref(L, LUA_REGISTRYINDEX, sr->func_error_ref);

	if (dolock) li_lua_unlock(srv);
}

static void subvr_bind_lua(liSubrequest *sr, lua_State *L, int notify_ndx, int error_ndx) {
	sr->L = L;

	lua_pushvalue(L, notify_ndx); /* +1 */
	sr->func_notify_ref = luaL_ref(L, LUA_REGISTRYINDEX); /* -1 */

	lua_pushvalue(L, error_ndx); /* +1 */
	sr->func_error_ref = luaL_ref(L, LUA_REGISTRYINDEX); /* -1 */
}

static void subvr_check(liVRequest *vr) {
	liSubrequest *sr = LI_CONTAINER_OF(vr->coninfo, liSubrequest, coninfo);

	if (sr->notified_out_bytes < vr->vr_out->bytes_in
	 || sr->notified_out_closed != vr->vr_out->is_closed
	 || sr->notified_response_headers != sr->have_response_headers) {
		subvr_run_lua(sr, sr->func_notify_ref);
	}

	sr->notified_out_bytes = vr->vr_out->bytes_in;
	sr->notified_out_closed = vr->vr_out->is_closed;
	sr->notified_response_headers = sr->have_response_headers;

	if (sr->notified_out_closed) { /* reques done */
		li_job_async(sr->parentvr_ref);
	}
}

static G_GNUC_WARN_UNUSED_RESULT gboolean subvr_handle_response_headers(liVRequest *vr) {
	liSubrequest *sr = LI_CONTAINER_OF(vr->coninfo, liSubrequest, coninfo);
	sr->have_response_headers = TRUE;

	subvr_check(vr);

	return TRUE;
}

static G_GNUC_WARN_UNUSED_RESULT gboolean subvr_handle_response_body(liVRequest *vr) {
	subvr_check(vr);

	return TRUE;
}

static G_GNUC_WARN_UNUSED_RESULT gboolean subvr_handle_response_error(liVRequest *vr) {
	liSubrequest *sr = LI_CONTAINER_OF(vr->coninfo, liSubrequest, coninfo);

	li_vrequest_free(sr->vr);
	sr->vr = NULL;

	subvr_run_lua(sr, sr->func_error_ref);
	subvr_release_lua(sr);

	return FALSE;
}

static G_GNUC_WARN_UNUSED_RESULT gboolean subvr_handle_request_headers(liVRequest *vr) {
	subvr_check(vr);

	return TRUE;
}

static gboolean subvr_handle_check_io(liVRequest *vr) {
	subvr_check(vr);

	return TRUE;
}

const liConCallbacks subrequest_callbacks = {
	subvr_handle_request_headers,
	subvr_handle_response_headers,
	subvr_handle_response_body,
	subvr_handle_response_error,

	subvr_handle_check_io
};

static liSubrequest* subrequest_new(liVRequest *vr) {
	liSubrequest* sr = g_slice_new0(liSubrequest);

	sr->wrk = vr->wrk;
	sr->parentvr_ref = li_vrequest_get_ref(vr);

	/* duplicate coninfo */
	sr->coninfo.callbacks = &subrequest_callbacks;
	sr->coninfo.remote_addr = li_sockaddr_dup(vr->coninfo->remote_addr);
	sr->coninfo.local_addr = li_sockaddr_dup(vr->coninfo->local_addr);
	sr->coninfo.remote_addr_str = g_string_new_len(GSTR_LEN(vr->coninfo->remote_addr_str));
	sr->coninfo.local_addr_str = g_string_new_len(GSTR_LEN(vr->coninfo->local_addr_str));
	sr->coninfo.is_ssl = vr->coninfo->is_ssl;
	sr->coninfo.keep_alive = FALSE; /* doesn't mean anything here anyway */

	sr->vr = li_vrequest_new(vr->wrk, &sr->coninfo);

	li_vrequest_start(sr->vr);

	li_request_copy(&sr->vr->request, &vr->request);

	sr->vr->request.content_length = 0;
	sr->vr->vr_in->is_closed = TRUE;

	return sr;
}

static int lua_subrequest_gc(lua_State *L) {
	liSubrequest *sr = li_lua_get_subrequest(L, 1);
	liServer *srv = sr->wrk->srv;
	gboolean dolock = (L == srv->L);

	sr->L = NULL;

	if (NULL == sr) return 0;
	if (dolock) li_lua_unlock(srv); /* "pause" lua */

	if (sr->vr) {
		li_vrequest_free(sr->vr);
		sr->vr = NULL;
	}

	li_sockaddr_clear(&sr->coninfo.remote_addr);
	li_sockaddr_clear(&sr->coninfo.local_addr);
	g_string_free(sr->coninfo.remote_addr_str, TRUE);
	g_string_free(sr->coninfo.local_addr_str, TRUE);

	g_slice_free(liSubrequest, sr);

	li_job_async(sr->parentvr_ref);
	li_job_ref_release(sr->parentvr_ref);

	if (dolock) li_lua_lock(srv);

	return 0;
}

int li_lua_vrequest_subrequest(lua_State *L) {
	liVRequest *vr;
	liSubrequest *sr;
	liAction *a;
	
	vr = li_lua_get_vrequest(L, 1);
	if (NULL == vr) return 0;

	a = li_lua_get_action_ref(L, 2);
	if (a == NULL) a = vr->wrk->srv->mainaction;

	sr = subrequest_new(vr);
	if (NULL == sr) return 0;

	subvr_bind_lua(sr, L, 3, 4);

	li_action_enter(sr->vr, a);
	li_vrequest_handle_request_headers(sr->vr);

	return li_lua_push_subrequest(L, sr);
}
