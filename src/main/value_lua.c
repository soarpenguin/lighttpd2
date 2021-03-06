
#include <lighttpd/value_lua.h>
#include <lighttpd/condition_lua.h>
#include <lighttpd/actions_lua.h>
#include <lighttpd/core_lua.h>

static liValue* li_value_from_lua_table(liServer *srv, lua_State *L, int ndx) {
	liValue *val = NULL, *sub_option;
	GArray *list = NULL;
	GHashTable *hash = NULL;
	int ikey;
	GString *skey;

	ndx = li_lua_fixindex(L, ndx);
	lua_pushnil(L);
	while (lua_next(L, ndx) != 0) {
		switch (lua_type(L, -2)) {
		case LUA_TNUMBER:
			if (hash) goto mixerror;
			if (!list) {
				val = li_value_new_list();
				list = val->data.list;
			}
			ikey = lua_tointeger(L, -2) - 1;
			if (ikey < 0) {
				ERROR(srv, "Invalid key < 0: %i - skipping entry", ikey + 1);
				lua_pop(L, 1);
				continue;
			}
			sub_option = li_value_from_lua(srv, L);
			if (!sub_option) continue;
			if ((size_t) ikey >= list->len) {
				g_array_set_size(list, ikey + 1);
			}
			g_array_index(list, liValue*, ikey) = sub_option;
			break;

		case LUA_TSTRING:
			if (list) goto mixerror;
			if (!hash) {
				val = li_value_new_hash();
				hash = val->data.hash;
			}
			skey = li_lua_togstring(L, -2);
			if (g_hash_table_lookup(hash, skey)) {
				ERROR(srv, "Key already exists in hash: '%s' - skipping entry", skey->str);
				lua_pop(L, 1);
				continue;
			}
			sub_option = li_value_from_lua(srv, L);
			if (!sub_option) {
				g_string_free(skey, TRUE);
				continue;
			}
			g_hash_table_insert(hash, skey, sub_option);
			break;

		default:
			ERROR(srv, "Unexpted key type in table: %s (%i) - skipping entry", lua_typename(L, -1), lua_type(L, -1));
			lua_pop(L, 1);
			break;
		}
	}

	return val;

mixerror:
	ERROR(srv, "%s", "Cannot mix list with hash; skipping remaining part of table");
	lua_pop(L, 2);
	return val;
}


liValue* li_value_from_lua(liServer *srv, lua_State *L) {
	liValue *val;

	switch (lua_type(L, -1)) {
	case LUA_TNIL:
		lua_pop(L, 1);
		return NULL;

	case LUA_TBOOLEAN:
		val = li_value_new_bool(lua_toboolean(L, -1));
		lua_pop(L, 1);
		return val;

	case LUA_TNUMBER:
		val = li_value_new_number(lua_tonumber(L, -1));
		lua_pop(L, 1);
		return val;

	case LUA_TSTRING:
		val = li_value_new_string(li_lua_togstring(L, -1));
		lua_pop(L, 1);
		return val;

	case LUA_TTABLE:
		val = li_value_from_lua_table(srv, L, -1);
		lua_pop(L, 1);
		return val;

	case LUA_TUSERDATA:
		{ /* check for action */
			liAction *a = li_lua_get_action(L, -1);
			if (a) {
				li_action_acquire(a);
				lua_pop(L, 1);
				return li_value_new_action(srv, a);
			}
		}
		{ /* check for condition */
			liCondition *c = li_lua_get_condition(L, -1);
			if (c) {
				li_condition_acquire(c);
				lua_pop(L, 1);
				return li_value_new_condition(srv, c);
			}
		}
		ERROR(srv, "%s", "Unknown lua userdata");
		lua_pop(L, 1);
		return NULL;

	case LUA_TFUNCTION: {
			liAction *a = li_lua_make_action(L, -1);
			lua_pop(L, 1);
			return li_value_new_action(srv, a);
		}

	case LUA_TLIGHTUSERDATA:
	case LUA_TTHREAD:
	case LUA_TNONE:
	default:
		ERROR(srv, "Unexpected lua type: %s (%i)", lua_typename(L, -1), lua_type(L, -1));
		lua_pop(L, 1);
		return NULL;
	}
}

GString* li_lua_togstring(lua_State *L, int ndx) {
	const char *buf;
	size_t len = 0;
	GString *str = NULL;

	if (lua_type(L, ndx) == LUA_TSTRING) {
		buf = lua_tolstring(L, ndx, &len);
		if (buf) str = g_string_new_len(buf, len);
	} else {
		lua_pushvalue(L, ndx);
		buf = lua_tolstring(L, -1, &len);
		if (buf) str = g_string_new_len(buf, len);
		lua_pop(L, 1);
	}

	return str;
}

int li_lua_push_value(lua_State *L, liValue *value) {
	if (NULL == value) {
		lua_pushnil(L);
		return 1;
	}

	switch (value->type) {
	case LI_VALUE_NONE:
		lua_pushnil(L);
		break;
	case LI_VALUE_BOOLEAN:
		lua_pushboolean(L, value->data.boolean);
		break;
	case LI_VALUE_NUMBER:
		lua_pushinteger(L, value->data.number);
		break;
	case LI_VALUE_STRING:
		lua_pushlstring(L, GSTR_LEN(value->data.string));
		break;
	case LI_VALUE_LIST: {
		GArray *list = value->data.list;
		guint i;
		lua_newtable(L);
		for (i = 0; i < list->len; i++) {
			liValue *subval = g_array_index(list, liValue*, i);
			li_lua_push_value(L, subval);
			lua_rawseti(L, -2, i);
		}
	} break;
	case LI_VALUE_HASH: {
		GHashTableIter it;
		gpointer pkey, pvalue;
		lua_newtable(L);

		g_hash_table_iter_init(&it, value->data.hash);
		while (g_hash_table_iter_next(&it, &pkey, &pvalue)) {
			GString *key = pkey;
			liValue *subval = pvalue;
			lua_pushlstring(L, GSTR_LEN(key));
			li_lua_push_value(L, subval);
			lua_rawset(L, -3);
		}
	} break;
	case LI_VALUE_ACTION:
		li_action_acquire(value->data.val_action.action);
		li_lua_push_action(value->data.val_action.srv, L, value->data.val_action.action);
		break;
	case LI_VALUE_CONDITION:
		li_condition_acquire(value->data.val_cond.cond);
		li_lua_push_condition(value->data.val_cond.srv, L, value->data.val_cond.cond);
		break;
	default: /* ignore error and push nil */
		lua_pushnil(L);
		break;
	}
	return 1;
}
