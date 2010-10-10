/*
 * mod_ssi -
 *
 * Description:
 *     mod_ssi
 *
 * Setups:
       none
 * Options:
 * Actions:
 *
 * Example config:
 *
 * Tip:
 *     none
 *
 * Todo:
 *
 * Author:
 *     Copyright (c) 2010 Stefan Bühler
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>

LI_API gboolean mod_ssi_init(liModules *mods, liModule *mod);
LI_API gboolean mod_ssi_free(liModules *mods, liModule *mod);



static const liPluginOption options[] = {
	{ NULL, 0, 0, NULL }
};

static const liPluginOptionPtr optionptrs[] = {
	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};


static void plugin_ssi_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->optionptrs = optionptrs;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_ssi_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_ssi", plugin_ssi_init, NULL);

	return mod->config != NULL;
}

gboolean mod_ssi_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}
