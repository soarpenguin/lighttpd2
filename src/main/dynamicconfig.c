
#include <lighttpd/dynamicconfig.h>

#define ENTER(x) do { g_atomic_int_inc(&(x)->refcount); } while(0)
#define LEAVE(x, destroy) do { if (g_atomic_int_dec_and_test(&(x)->refcount)) destroy(x); } while(0)
#define _LEAVE(x) do { if (g_atomic_int_dec_and_test(&(x)->refcount)) assert(FALSE); } while(0)

struct liDynamicConfig {
	int refcount;

	liServer *srv;
	struct ev_loop *loop;
	ev_async update_watcher;
	ev_timer max_ttl_watcher;
	gboolean shutdown;
	gboolean watchers_active; /* keeps a reference */

	const liDynamicConfigCallbacks *callbacks;

	ev_tstamp recheck_hit_ttl, recheck_miss_ttl, max_hit_ttl, max_miss_ttl; /* if recheck < 0 => max < 0; otherwise max > recheck */
	ev_tstamp max_ttl; /* == max(max_*_ttl, 60) */

	GMutex *lock; /* functions prefixed with a "_" must be called while the lock is held (and a "stack" reference to dynconf) */

	GQueue max_ttl_queue;

	GHashTable *entries; /* keeps a reference to the entries */

	liAction *miss_action;
	gpointer param;
};

struct liDynamicConfigEntry {
	int refcount;

	GString *key;

	ev_tstamp last_update, last_lookup;

	liDynamicConfig *dynconf; /* keeps a reference */

	gboolean valid; /* == entry in dynconf->entries, keeps a reference (together with entries) */
	gboolean active; /* FALSE if this is a new entry (never got an action). active == FALSE => lookup_running == TRUE */
	GList max_ttl_elem;

	gboolean lookup_running; /* keeps a reference */

	liAction *action;

	GQueue jobrefs;
	gpointer data;
};

typedef struct dc_context dc_context;
struct dc_context {
	GList jobrefs_elem;
	liDynamicConfigEntry *entry;
	gint tries;
};

static void _entry_free(liDynamicConfigEntry *entry);
static void dynconf_free(liDynamicConfig *dyncon);

static void _entry_wakeup(liDynamicConfigEntry *entry) {
	GList *iter;

	while (NULL != (iter = g_queue_peek_head_link(&entry->jobrefs))) {
		liJobRef* jobref = iter->data;
		dc_context *ctx = LI_CONTAINER_OF(iter, dc_context, jobrefs_elem);

		g_queue_unlink(&entry->jobrefs, iter);

		iter->data = NULL;
		ctx->entry = NULL;

		li_job_async(jobref);
		li_job_ref_release(jobref);
	}
}

static void _entry_free(liDynamicConfigEntry *entry) {
	liDynamicConfig *dynconf;

	if (NULL == entry) return;

	ENTER(entry); /* don't reenter */

	dynconf = entry->dynconf;
	entry->valid = FALSE;

	if (NULL != entry->action) {
		g_mutex_unlock(entry->dynconf->lock);
		li_action_release(entry->dynconf->srv, entry->action);
		entry->action = NULL;
		g_mutex_lock(entry->dynconf->lock);
	}

	if (entry->jobrefs.length > 0) {
		_entry_wakeup(entry);
	}

	if (NULL != entry->max_ttl_elem.data) {
		g_queue_unlink(&dynconf->max_ttl_queue, &entry->max_ttl_elem);
		entry->max_ttl_elem.data = NULL;
	}

	g_mutex_unlock(entry->dynconf->lock);

	if (NULL != dynconf->callbacks->free_entry) {
		dynconf->callbacks->free_entry(entry->key, dynconf->param, entry->data);
	}

	g_mutex_lock(entry->dynconf->lock);

	g_string_free(entry->key, TRUE);
	_LEAVE(entry->dynconf);

	g_slice_free(liDynamicConfigEntry, entry);
}

static gboolean _entry_usable(liDynamicConfig *dynconf, liDynamicConfigEntry *entry, ev_tstamp now) {
	return entry->valid && entry->active && (NULL == entry->action
		? (dynconf->max_miss_ttl < 0 || entry->last_update + dynconf->max_miss_ttl > now)
		: (dynconf->max_hit_ttl < 0 || entry->last_update + dynconf->max_hit_ttl > now));
}

static void dynconf_free(liDynamicConfig *dynconf) {
	ENTER(dynconf); /* don't reenter */

	li_action_release(dynconf->srv, dynconf->miss_action);
	dynconf->miss_action = NULL;

	assert(0 == g_hash_table_size(dynconf->entries)); /* every entry keeps the dynconf alive! */
	g_hash_table_destroy(dynconf->entries);

	if (NULL != dynconf->callbacks->free_param) {
		dynconf->callbacks->free_param(dynconf->param);
	}

	g_mutex_free(dynconf->lock);
	dynconf->lock = NULL;

	g_slice_free(liDynamicConfig, dynconf);
}

static void _dynconf_update(liDynamicConfig *dynconf) {
	ev_tstamp now = ev_now(dynconf->loop);
	liDynamicConfigEntry *entry;

	if (dynconf->shutdown && (!dynconf->srv->worker_threads_running || 2 == dynconf->refcount)) {
		if (dynconf->watchers_active) {
			dynconf->watchers_active = FALSE;
			ev_timer_stop(dynconf->loop, &dynconf->max_ttl_watcher);
			ev_async_stop(dynconf->loop, &dynconf->update_watcher);
			_LEAVE(dynconf);
		}
	}

	while (NULL != (entry = g_queue_peek_head(&dynconf->max_ttl_queue))) {
		if (_entry_usable(dynconf, entry, now)) break;

		g_queue_unlink(&dynconf->max_ttl_queue, &entry->max_ttl_elem);
		entry->max_ttl_elem.data = NULL;

		if (entry->valid) {
			g_hash_table_steal(dynconf->entries, entry->key);
			entry->valid = FALSE;
		}
		_entry_wakeup(entry);

		LEAVE(entry, _entry_free);
	}

	if (dynconf->watchers_active) {
		if (dynconf->max_ttl_queue.length > 0) {
			entry = g_queue_peek_head(&dynconf->max_ttl_queue);
			dynconf->max_ttl_watcher.repeat = entry->last_update + dynconf->max_ttl - ev_now(dynconf->loop);
			ev_timer_again(dynconf->loop, &dynconf->max_ttl_watcher);
		} else {
			ev_timer_stop(dynconf->loop, &dynconf->max_ttl_watcher);
		}
	}
}

static void dynconf_update_watcher_cb(struct ev_loop *loop, ev_async *w, int revents) {
	liDynamicConfig *dynconf = w->data;
	UNUSED(loop);
	UNUSED(revents);

	ENTER(dynconf);
	g_mutex_lock(dynconf->lock);
	_dynconf_update(dynconf);
	g_mutex_unlock(dynconf->lock);
	LEAVE(dynconf, dynconf_free);
}

static void dynconf_max_ttl_watcher_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	liDynamicConfig *dynconf = w->data;
	UNUSED(loop);
	UNUSED(revents);

	ENTER(dynconf);
	g_mutex_lock(dynconf->lock);
	_dynconf_update(dynconf);
	g_mutex_unlock(dynconf->lock);
	LEAVE(dynconf, dynconf_free);
}

static void _dynconf_update_async(liDynamicConfig *dynconf) {
	if (dynconf->srv->worker_threads_running && dynconf->watchers_active) {
		ev_async_send(dynconf->loop, &dynconf->update_watcher);
	} else {
		_dynconf_update(dynconf);
	}
}

/* FALSE means entry isn't valid anymore */
static gboolean _entry_update(liDynamicConfig *dynconf, liDynamicConfigEntry *entry, ev_tstamp now) {
	gboolean res;

	if (entry->lookup_running) return TRUE;
	if (!entry->valid) return FALSE;

	if (entry->active && (NULL == entry->action
		? (dynconf->recheck_miss_ttl < 0 || entry->last_update + dynconf->recheck_miss_ttl > now)
		: (dynconf->recheck_hit_ttl < 0 || entry->last_update + dynconf->recheck_hit_ttl > now)))
		return TRUE;


	ENTER(entry); /* to keep it alive while we unlock dyncon */

	ENTER(entry); /* lookup reference */
	entry->lookup_running = TRUE;
	entry->last_lookup = now;

	if (NULL != entry->max_ttl_elem.data) {
		g_queue_unlink(&dynconf->max_ttl_queue, &entry->max_ttl_elem);
		entry->max_ttl_elem.data = NULL;
	}

	g_mutex_unlock(dynconf->lock);

	dynconf->callbacks->lookup(entry, entry->key, entry->last_update, dynconf->param, &entry->data);

	g_mutex_lock(dynconf->lock);

	res = (entry->refcount > 1) && entry->valid;

	LEAVE(entry, _entry_free);

	return res;
}

static liDynamicConfigEntry* _entry_new(liDynamicConfig *dynconf, GString *key, ev_tstamp now) {
	liDynamicConfigEntry *entry = g_slice_new0(liDynamicConfigEntry);

	entry->refcount = 1; /* hashtable entry reference */
	entry->key = g_string_new_len(GSTR_LEN(key));
	entry->last_update = entry->last_lookup = now;

	ENTER(dynconf);
	entry->dynconf = dynconf;

	entry->valid = TRUE;
	entry->active = FALSE;

	g_hash_table_insert(dynconf->entries, entry->key, entry);

	if (!_entry_update(dynconf, entry, now)) return NULL;

	return entry;
}

static void _entry_select(liVRequest *vr, liDynamicConfig *dynconf, liDynamicConfigEntry *entry) {
	liAction *a = NULL != entry->action ? entry->action : dynconf->miss_action;

	if (NULL != a) li_action_enter(vr, a);
}

liDynamicConfig *li_dyncon_new(liServer* srv, liWorker* curwrk, const liDynamicConfigCallbacks* callbacks, liAction* miss_action, gpointer param, ev_tstamp recheck_hit_ttl, ev_tstamp recheck_miss_ttl, ev_tstamp max_hit_ttl, ev_tstamp max_miss_ttl) {
	liDynamicConfig *dynconf = g_slice_new0(liDynamicConfig);

	dynconf->refcount = 2; /* watchers_active + returned reference */
	dynconf->srv = srv;
	dynconf->loop = curwrk->loop;

	ev_async_init(&dynconf->update_watcher, dynconf_update_watcher_cb);
	dynconf->update_watcher.data = dynconf;
	ev_async_start(dynconf->loop, &dynconf->update_watcher);

	ev_timer_init(&dynconf->max_ttl_watcher, dynconf_max_ttl_watcher_cb, 0.0, 0.0);
	dynconf->max_ttl_watcher.data = dynconf;

	dynconf->shutdown = FALSE;
	dynconf->watchers_active = TRUE;

	dynconf->callbacks = callbacks;

	dynconf->recheck_hit_ttl = recheck_hit_ttl;
	dynconf->recheck_miss_ttl = recheck_miss_ttl;
	dynconf->max_hit_ttl = max_hit_ttl;
	dynconf->max_miss_ttl = max_miss_ttl;
	dynconf->max_ttl = MAX(60, MAX(max_hit_ttl, max_miss_ttl));

	dynconf->lock = g_mutex_new();

	dynconf->entries = g_hash_table_new((GHashFunc) g_string_hash, (GEqualFunc) g_string_equal);

	dynconf->miss_action = miss_action;
	dynconf->param = param;

	return dynconf;
}


void li_dyncon_free(liDynamicConfig *dynconf) {
	if (NULL == dynconf) return;

	g_mutex_lock(dynconf->lock);

	assert(!dynconf->shutdown);

	dynconf->shutdown = TRUE;

	/* invalidate all entries */
	{
		GHashTableIter iter;
		GQueue entries;
		gpointer pkey, pvalue;
		liDynamicConfigEntry *entry;

		/* we can't free the entries directly, as that would unlock dynconf and someone else could be modifying the hashtable */

		g_queue_init(&entries);

		g_hash_table_iter_init(&iter, dynconf->entries);
		while (g_hash_table_iter_next(&iter, &pkey, &pvalue)) {
			entry = pvalue;

			if (NULL != entry->max_ttl_elem.data) {
				g_queue_unlink(&dynconf->max_ttl_queue, &entry->max_ttl_elem);
				entry->max_ttl_elem.data = NULL;
			}

			entry->valid = FALSE;
			_entry_wakeup(entry);

			if (entry->refcount > 1) {
				_LEAVE(entry);
			} else {
				/* abuse max_ttl_elem temporarily; we have the last reference anyway, nobody else is using it */
				entry->max_ttl_elem.data = entry;
				g_queue_push_tail_link(&entries, &entry->max_ttl_elem);
			}
		}

		g_hash_table_steal_all(dynconf->entries);

		while (NULL != (entry = g_queue_peek_head(&entries))) {
			g_queue_unlink(&entries, &entry->max_ttl_elem);
			entry->max_ttl_elem.data = NULL;

			LEAVE(entry, _entry_free);
		}
	}

	_dynconf_update_async(dynconf);

	g_mutex_unlock(dynconf->lock);

	LEAVE(dynconf, dynconf_free);
}

void li_dyncon_invalidate(liDynamicConfig *dynconf, GString *key) {
	liDynamicConfigEntry *entry;

	ENTER(dynconf);

	g_mutex_lock(dynconf->lock);

	entry = g_hash_table_lookup(dynconf->entries, key);

	if (NULL != entry) {
		entry->valid = FALSE;
		_entry_wakeup(entry);

		g_hash_table_steal(dynconf->entries, key);
		LEAVE(entry, _entry_free);
	}

	g_mutex_unlock(dynconf->lock);

	LEAVE(dynconf, dynconf_free);
}

static void _wait_vr(liDynamicConfigEntry *entry, liVRequest *vr, gpointer *context) {
	dc_context *ctx = *context;

	assert(entry->lookup_running);

	if (NULL == ctx) {
		*context = ctx = g_slice_new0(dc_context);
		ctx->tries = 1;
	}

	if (NULL == ctx->jobrefs_elem.data) {
		ctx->jobrefs_elem.data = li_vrequest_get_ref(vr);
		g_queue_push_tail_link(&entry->jobrefs, &ctx->jobrefs_elem);
	}
}

static void _unwait_vr(gpointer *context) {
	dc_context *ctx = *context;

	if (NULL == ctx) return;

	*context = NULL;

	if (NULL == ctx->jobrefs_elem.data) {
		liDynamicConfigEntry *entry = ctx->entry;
		assert(NULL != entry);

		li_job_ref_release(ctx->jobrefs_elem.data);
		ctx->jobrefs_elem.data = NULL;
		g_queue_unlink(&entry->jobrefs, &ctx->jobrefs_elem);
	}

	g_slice_free(dc_context, ctx);
}

liHandlerResult li_dyncon_handle(liVRequest *vr, liDynamicConfig *dynconf, gpointer *context, GString *key) {
	dc_context *ctx = *context;
	ev_tstamp now = ev_now(vr->wrk->loop);
	liDynamicConfigEntry *entry = NULL;

	ENTER(dynconf);

	g_mutex_lock(dynconf->lock);

	if (NULL != ctx && NULL != ctx->entry && ctx->entry->valid) {
		entry = ctx->entry;
		ctx->tries--; /* not a "real" try as entry wasn't lost */

		if (_entry_update(dynconf, entry, now)) {
			if (!_entry_usable(dynconf, entry, now)) {
				/* wait for update */
				goto wait;
			} else {
				_entry_select(vr, dynconf, entry);
				goto done;
			}
		}
	}

	entry = g_hash_table_lookup(dynconf->entries, key);

	if (NULL != entry) {
		if (_entry_update(dynconf, entry, now)) {
			if (!_entry_usable(dynconf, entry, now)) {
				/* wait for update */
				goto wait;
			} else {
				_entry_select(vr, dynconf, entry);
				goto done;
			}
		}
	} else {
		/* create new entry */
		entry = _entry_new(dynconf, key, now);
		if (NULL != entry) {
			if (!_entry_usable(dynconf, entry, now)) {
				/* wait for update */
				goto wait;
			} else {
				_entry_select(vr, dynconf, entry);
				goto done;
			}
		}
	}

failed:
	VR_ERROR(vr, "Couldn't get a valid entry for '%s'", key->str);
	li_vrequest_error(vr);

	goto done;

wait:
	if (ctx != NULL) {
		ctx->tries++;
		if (ctx->tries > 2) goto failed;
	}

	_wait_vr(entry, vr, context);
	g_mutex_unlock(dynconf->lock);

	LEAVE(dynconf, dynconf_free);

	return LI_HANDLER_WAIT_FOR_EVENT;

done:
	_unwait_vr(context);
	g_mutex_unlock(dynconf->lock);

	LEAVE(dynconf, dynconf_free);

	return LI_HANDLER_GO_ON;
}

void li_dyncon_handle_cleanup(liVRequest *vr, liDynamicConfig *dynconf, gpointer context) {
	UNUSED(vr);

	if (context == NULL) return;

	ENTER(dynconf);

	g_mutex_lock(dynconf->lock);

	_unwait_vr(&context);

	g_mutex_unlock(dynconf->lock);

	LEAVE(dynconf, dynconf_free);
}

static void _entry_queue(liDynamicConfig *dynconf, liDynamicConfigEntry *entry) {
	/* unlink */
	if (NULL != entry->max_ttl_elem.data) {
		g_queue_unlink(&dynconf->max_ttl_queue, &entry->max_ttl_elem);
	}

	/* queue */
	entry->max_ttl_elem.data = entry;
	g_queue_push_tail_link(&dynconf->max_ttl_queue, &entry->max_ttl_elem);

	_dynconf_update_async(dynconf);
}

void li_dyncon_notify_continue(liDynamicConfigEntry *entry) {
	liDynamicConfig *dynconf = entry->dynconf;

	ENTER(dynconf);

	g_mutex_lock(dynconf->lock);

	assert(entry->lookup_running);

	entry->last_update = entry->last_lookup;

	_entry_wakeup(entry);
	_entry_queue(dynconf, entry);

	entry->lookup_running = FALSE;
	LEAVE(entry, _entry_free);

	g_mutex_unlock(dynconf->lock);

	LEAVE(dynconf, dynconf_free);
}

void li_dyncon_notify_update(liDynamicConfigEntry *entry, liAction *action) {
	liDynamicConfig *dynconf = entry->dynconf;
	liAction *oldaction = NULL;

	ENTER(dynconf);

	g_mutex_lock(dynconf->lock);

	assert(entry->lookup_running);

	entry->last_update = entry->last_lookup;
	oldaction = entry->action;
	entry->action = action;
	entry->active = TRUE;

	_entry_wakeup(entry);
	_entry_queue(dynconf, entry);

	entry->lookup_running = FALSE;
	LEAVE(entry, _entry_free);

	g_mutex_unlock(dynconf->lock);

	if (NULL != oldaction) {
		li_action_release(dynconf->srv, oldaction);
	}

	LEAVE(dynconf, dynconf_free);
}
