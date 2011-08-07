/*
 * mod_auth_dovecot - require authentication from clients using username + password
 *
 * Description:
 *     mod_auth lets you require authentication from clients using a username and password.
 *     dovecot is used as backend
 *
 * Setups:
 *     none
 * Options:
 *     auth.debug = <true|false>;
 *         - if set, debug information is written to the log
 * Actions:
 *     auth.plain ["method": method, "realm": realm, "file": path, "ttl": 10];
 *         - requires authentication using a plaintext file containing user:password pairs seperated by newlines (\n)
 *     auth.htpasswd ["method": method, "realm": realm, "file": path, "ttl": 10];
 *         - requires authentication using a htpasswd file containing user:encrypted_password pairs seperated by newlines (\n)
 *         - passwords are encrypted using crypt(3), use the htpasswd binary from apache to manage the file
 *           + hashes starting with "$apr1$" ARE supported (htpasswd -m)
 *           + hashes starting with "{SHA}" ARE supported (followed by sha1_base64(password), htpasswd -s)
 *         - only supports "basic" method
 *     auth.htdigest ["method": method, "realm": realm, "file": path, "ttl": 10];
 *         - requires authentication using a htdigest file containing user:realm:hashed_password tuples seperated by newlines (\n)
 *         - passwords are saved as (modified) md5 hashes:
 *             md5hex(username + ":" + realm + ":" + password)
 *
 *     ttl specifies how often lighty checks the files for modifications (in seconds), 0 means it will never check after the first load.
 *
 *     auth.deny;
 *         - handles request with "401 Unauthorized"
 *
 * Example config:
 *     # /members/ is for known users only
 *     if request.path =^ "/members/" {
 *         auth.plain ["method": "basic", "realm": "members only", "file": "/etc/lighttpd/users.txt"];
 *     }
 *     if req.env["REMOTE_USER"] !~ "^(admin1|user2|user3)$" { auth.deny; }
 *
 *
 * Author:
 *     Copyright (c) 2011 Stefan Bühler
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <lighttpd/encoding.h>

#include <lighttpd/plugin_core.h>

#include <lighttpd/dovecot_auth_parser.h>

LI_API gboolean mod_auth_dovceot_init(liModules *mods, liModule *mod);
LI_API gboolean mod_auth_dovceot_free(liModules *mods, liModule *mod);

static liHandlerResult auth_handle(liVRequest *vr, gpointer param, gpointer *context) {
	liPlugin *p = param;
	UNUSED(context);

	if (!li_vrequest_handle_direct(vr)) {
		if (_OPTION(vr, p, 0).boolean || CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "skipping auth.deny as request is already handled with current status %i", vr->response.http_status);
		}
		return LI_HANDLER_GO_ON;
	}

	vr->response.http_status = 403;

	return LI_HANDLER_GO_ON;
}

static liAction* auth_dovecot_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(srv);
	UNUSED(wrk);
	UNUSED(userdata);

	if (val) {
		ERROR(srv, "%s", "'auth_dovecot' action doesn't have parameters");
		return NULL;
	}

	return li_action_new_function(auth_handle, NULL, NULL, p);
}

static const liPluginOption options[] = {
	{ "auth_dovecot.debug", LI_VALUE_BOOLEAN, 0, NULL },

	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "auth_dovecot", auth_dovecot_create, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};

static void plugin_auth_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv);
	UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_auth_dovceot_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_auth_dovecot", plugin_auth_init, NULL);

	return mod->config != NULL;
}

gboolean mod_auth_dovceot_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}
