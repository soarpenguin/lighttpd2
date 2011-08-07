#ifndef _LIGHTTPD_DYNAMICCONFIG_H_
#define _LIGHTTPD_DYNAMICCONFIG_H_

#include <lighttpd/base.h>

typedef struct liDynamicConfig liDynamicConfig;
typedef struct liDynamicConfigEntry liDynamicConfigEntry;
typedef struct liDynamicConfigCallbacks liDynamicConfigCallbacks;

struct liDynamicConfigCallbacks {
	void (*lookup)(liDynamicConfigEntry *entry, GString *key, ev_tstamp last_check, gpointer param, gpointer *data);
	void (*free_entry)(GString *key, gpointer param, gpointer data);

	void (*free_param)(gpointer param);
};

/* after recheck_*_ttl a "lookup" callback is triggered if a request ask for an entry;
 * after max_ttl an entry will be invalidated (either no response from lookup or no lookup was triggered
 * notify_continue/update resets the timestamps to the last "lookup" callback timestamp.
 *
 * takes ownership of miss_action (acquire it before if you want to use it yourself again)
 */
LI_API liDynamicConfig *li_dyncon_new(liServer *srv, liWorker *curwrk, const liDynamicConfigCallbacks *callbacks, liAction *miss_action, gpointer param, ev_tstamp recheck_hit_ttl, ev_tstamp recheck_miss_ttl, ev_tstamp max_hit_ttl, ev_tstamp max_miss_ttl);
LI_API void li_dyncon_free(liDynamicConfig *dynconf);

LI_API void li_dyncon_invalidate(liDynamicConfig *dynconf, GString *key);

LI_API liHandlerResult li_dyncon_handle(liVRequest *vr, liDynamicConfig *dynconf, gpointer *context, GString *key);
LI_API void li_dyncon_handle_cleanup(liVRequest *vr, liDynamicConfig *dynconf, gpointer context);

/* call this exactly *once* after you got a "lookup" callback for an entry */
LI_API void li_dyncon_notify_continue(liDynamicConfigEntry *entry); /* no changes */
LI_API void li_dyncon_notify_update(liDynamicConfigEntry *entry, liAction *action); /* loaded new action (action == NULL => no action available anymore) */

#endif
