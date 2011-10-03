#ifndef _LIGHTTPD_BACKENDS_H_
#define _LIGHTTPD_BACKENDS_H_

#include <lighttpd/base.h>

enum liBackendResult {
	LI_BACKEND_SUCCESS, /* got a connection */
	LI_BACKEND_WAIT, /* establishing new connection, or waiting for a free slot */
	LI_BACKEND_TIMEOUT /* wait timed out, no free slots available */
};

typedef struct liBackend liBackend;
typedef struct liBackendConnection liBackendConnection;
typedef enum liBackendResult liBackendResult;

typedef struct liBackendWait liBackendWait;

struct liBackend {
	liWorker *wrk;          /* "master" worker, needed for li_worker_add_closing_socket */
	struct ev_loop *loop;   /* loop from ->wrk, used for all internal watchers */

	GMutex *mutex;
	liSocketAddress sock_addr;

	int max_connections; /* -1: unlimited */
	GPtrArray *active_connections, *close_connections;
	guint idle_timeout, connect_timeout, wait_timeout, disable_time;
	int max_requests; /* -1: unlimited */

	/* waiting vrequests */
	GQueue vr_wait_queue, idle_queue;
	ev_tstamp ts_vr_wait, ts_idle, ts_connect, ts_disabled;
	ev_tstamp ts_timeout; /* new timeout */

	int cur_connect_fd;
	ev_io cur_connect_watcher;

	GPtrArray *update_backends;

	ev_async update_watcher;
	ev_periodic timeout_watcher;

	gboolean shutdown;
};

struct liBackendConnection {
	liBackend *backend;
	int fd;
	gint requests; /* read only */

	/* private */

	/* active: whether backend is in use
	 * watcher_active: whether io_watcher+signal_watcher are bound to a ev_loop
	 */
	gboolean active, closing;
	guint ndx; /* if !closing and active: index in active_connections */
	ev_io io_watcher;

	ev_tstamp ts_idle;
	GList idle_link;
};

LI_API liBackend* li_backend_new(liWorker *wrk, liSocketAddress sock_addr, gint max_connections, guint idle_timeout, guint connect_timeout, guint wait_timeout, guint disable_time, int max_requests);
LI_API void li_backend_free(liBackend *backend);

LI_API liBackendResult li_backend_get(liVRequest *vr, liBackend *backend, liBackendConnection **pbcon, liBackendWait **pbwait);
LI_API void li_backend_wait_stop(liBackend *backend, liBackendWait **pbwait);

/* set bcon->fd = -1 if you closed the connection after an error */
LI_API void li_backend_put(liBackendConnection *bcon, gboolean closecon); /* if close == TRUE or bcon->fd == -1 the connection gets removed */


/* example pseudo code: */
#if 0
try_connect() {
	switch (li_backend_get(vr, backend, &vrstate.bcon, &vrstate.bwait)) {
	case LI_BACKEND_SUCCESS:
		return LI_HANDLER_GO_ON;
	case LI_BACKEND_WAIT:
		return LI_HANDLER_WAIT_FOR_EVENT;
	case LI_BACKEND_TIMEOUT:
		if (!li_vrequest_handle_direct()) return LI_HANDLER_ERROR;
		vr->response.http_status = 504;
		return LI_HANDLER_GO_ON;
	}
	return LI_HANDLER_ERROR;
}

vr_reset() {
	if (vrstate.bwait) { /* only possible if last call to li_backend_get for our context returned LI_BACKEND_WAIT */
		li_backend_wait_stop(backend, &vrstate.bwait);
	}
	if (vrstate.bcon) {
		close(bcon->fd);
		bcon->fd = -1;
		li_backend_put(vrstate.bcon, TRUE);
	}
}

backend_done() {
	li_backend_put(vrstate.bcon, FALSE); /* unless the backend returned something like "Connection: close" there is probably no need to specify TRUE for closecon */
}
#endif

#endif
