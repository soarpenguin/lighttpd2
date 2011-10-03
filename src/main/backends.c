
#include <lighttpd/backends.h>

struct liBackendWait {
	GList link;
	ev_tstamp timeout; /* timeout = wait_timeout + insert timestamp */
	liJobRef *vr_ref;
};

static void backend_con_io_cb(struct ev_loop *loop, struct ev_io *w, int revents);

#define TS_CHECK(x) \
	do { if (0 != backend->x && (0 == timeout || backend->x < timeout)) timeout = backend->x; } while (0)

static void backend_recalc_timeout(liBackend *backend) {
	ev_tstamp timeout = 0;

	TS_CHECK(ts_vr_wait);
	TS_CHECK(ts_idle);
	TS_CHECK(ts_connect);
	TS_CHECK(ts_disabled);

	if (backend->ts_timeout != timeout) {
		backend->ts_timeout = timeout;
		ev_async_send(backend->loop, &backend->update_watcher);
	}
}

/* mutex has to be already locked!, thread context has to own backend->loop */
static void backend_master_recalc_timeout(liBackend *backend) {
	ev_tstamp timeout = 0;

	TS_CHECK(ts_vr_wait);
	TS_CHECK(ts_idle);
	TS_CHECK(ts_connect);
	TS_CHECK(ts_disabled);

	if (backend->ts_timeout != timeout) {
		backend->ts_timeout = timeout;
		backend->timeout_watcher.offset = backend->ts_timeout;
		if (backend->ts_timeout == 0) {
			ev_periodic_stop(backend->loop, &backend->timeout_watcher);
		} else {
			ev_periodic_again(backend->loop, &backend->timeout_watcher);
			/* libev might modify this value for better numerical stability  */
			backend->ts_timeout = backend->timeout_watcher.offset;
		}
	}
}
#undef TS_CHECK

/* mutex has to be already locked! */
static void backend_wakeup_vr(liBackend *backend) {
	liBackendWait *bwait;
	if (NULL != (bwait = g_queue_peek_head(&backend->vr_wait_queue))) {
		li_job_async(bwait->vr_ref);
		li_job_ref_release(bwait->vr_ref);
		bwait->vr_ref = NULL;
		g_queue_unlink(&backend->vr_wait_queue, &bwait->link);

		bwait = g_queue_peek_head(&backend->vr_wait_queue);
		backend->ts_vr_wait = (NULL != bwait) ? bwait->timeout : 0;
		backend_recalc_timeout(backend);
	}
}

/* mutex has to be already locked!, thread context has to own backend->loop */
static void backend_con_free(liBackend *backend, liBackendConnection *bcon) {
	ev_io_stop(backend->loop, &bcon->io_watcher);
	if (-1 != bcon->fd) {
		li_worker_add_closing_socket(backend->wrk, bcon->fd);
	}
	if (!bcon->closing) {
		if (bcon->active) {
			/* remove from active connections */
			((liBackendConnection*) g_ptr_array_index(backend->active_connections, bcon->ndx))->ndx = bcon->ndx;
			g_ptr_array_remove_index_fast(backend->active_connections, bcon->ndx);
		} else {
			/* remove from idle connections */
			g_queue_unlink(&backend->idle_queue, &bcon->idle_link);
		}
	}
	g_slice_free(liBackendConnection, bcon);
}

/* mutex has to be already locked!, if not active thread context has to own backend->loop */
static liBackendConnection* backend_con_new(liBackend *backend, int fd, gboolean active) {
	liBackendConnection *bcon = g_slice_new0(liBackendConnection);

	bcon->backend = backend;
	bcon->fd = fd;
	bcon->requests = 0;
	bcon->active = active;
	bcon->closing = FALSE;

	ev_io_init(&bcon->io_watcher, backend_con_io_cb, fd, EV_READ);
	bcon->io_watcher.data = bcon;

	bcon->idle_link.data = bcon;

	if (active) {
		bcon->ndx = backend->active_connections->len;
		g_ptr_array_add(backend->active_connections, bcon);
	} else {
		bcon->ndx = -1;

		/* add to idle queue */
		bcon->ts_idle = ev_time() + backend->idle_timeout;
		if (backend->idle_queue.length == 0) {
			backend->ts_idle = bcon->ts_idle;
			backend_master_recalc_timeout(backend);
		}
		g_queue_push_tail_link(&backend->idle_queue, &bcon->idle_link);

		ev_io_start(backend->loop, &bcon->io_watcher);
	}

	return bcon;
}

static void backend_update_cb(struct ev_loop *loop, struct ev_async *w, int revents) {
	liBackend *backend = w->data;
	guint i;
	UNUSED(loop);
	UNUSED(revents);

	g_mutex_lock(backend->mutex);

	/* update timer */
	if (backend->timeout_watcher.offset != backend->ts_timeout) {
		backend->timeout_watcher.offset = backend->ts_timeout;
		if (backend->ts_timeout == 0) {
			ev_periodic_stop(backend->loop, &backend->timeout_watcher);
		} else {
			ev_periodic_again(backend->loop, &backend->timeout_watcher);
			/* libev might modify this value for better numerical stability  */
			backend->ts_timeout = backend->timeout_watcher.offset;
		}
	}

	for (i = 0; i < backend->update_backends->len; i++) {
		liBackendConnection *bcon = g_ptr_array_index(backend->update_backends, i);
		if (bcon->active) {
			ev_io_stop(backend->loop, &bcon->io_watcher);
		} else {
			ev_io_start(backend->loop, &bcon->io_watcher);
		}
	}
	g_ptr_array_set_size(backend->update_backends, 0);

	/* cleanup closed/closing connections */
	for (i = 0; i < backend->close_connections->len; i++) {
		liBackendConnection *bcon = g_ptr_array_index(backend->close_connections, i);
		backend_con_free(backend, bcon);
	}
	g_ptr_array_set_size(backend->close_connections, 0);

	/* new connection watcher */
	if (backend->cur_connect_fd != backend->cur_connect_watcher.fd) {
		ev_io_set(&backend->cur_connect_watcher, backend->cur_connect_fd, EV_READ | EV_WRITE);
		ev_io_start(backend->loop, &backend->cur_connect_watcher);
		backend->ts_connect = CUR_TS(backend->wrk) + backend->connect_timeout;
		backend_master_recalc_timeout(backend);
	}


	if (backend->shutdown) {
		GList *elem;

		if (-1 != backend->cur_connect_fd) {
			close(backend->cur_connect_fd);
			ev_io_stop(backend->loop, &backend->cur_connect_watcher);
			backend->cur_connect_fd = -1;
			backend->cur_connect_watcher.fd = -1;
			backend->ts_connect = 0;
			backend_master_recalc_timeout(backend);
		}

		/* free all idle backends */
		while (NULL != (elem = g_queue_pop_head_link(&backend->idle_queue))) {
			liBackendConnection *bcon = elem->data;
			backend_con_free(backend, bcon);
		}

		if (0 == backend->active_connections->len && 0 == backend->ts_timeout) {
			/* ok, everything done - free backend */
			li_sockaddr_clear(&backend->sock_addr);
			g_ptr_array_free(backend->active_connections, TRUE);
			g_ptr_array_free(backend->update_backends, TRUE);
			g_ptr_array_free(backend->close_connections, TRUE);
			ev_async_stop(backend->loop, &backend->update_watcher);
			ev_periodic_stop(backend->loop, &backend->timeout_watcher);

			g_mutex_unlock(backend->mutex);
			g_mutex_free(backend->mutex);
			g_slice_free(liBackend, backend);
			return;
		}
	}

	g_mutex_unlock(backend->mutex);
}

static void backend_timeout_cb(struct ev_loop *loop, struct ev_periodic *w, int revents) {
	liBackend *backend = w->data;
	ev_tstamp now = ev_now(loop);
	UNUSED(revents);

	g_mutex_lock(backend->mutex);

	if (0 != backend->ts_connect && now >= backend->ts_connect) {
		ev_tstamp cts = backend->ts_connect;
		backend->ts_connect = 0;
		/* stop watching */
		close(backend->cur_connect_fd);
		backend->cur_connect_fd = -1;
		ev_io_stop(backend->loop, &backend->cur_connect_watcher);
		backend->cur_connect_watcher.fd = -1;

		ERROR(backend->wrk->srv, "Couldn't connect to '%s': Timeout",
			li_sockaddr_to_string(backend->sock_addr, backend->wrk->tmp_str, TRUE)->str);

		backend->ts_disabled = cts - backend->connect_timeout + backend->disable_time; /* disable time is relative to original connect start */
	}

	if (0 != backend->ts_disabled && now >= backend->ts_disabled) {
		backend->ts_disabled = 0;
		backend_wakeup_vr(backend); /* if a vr is still waiting it will try a new connect() */
	}

	if (0 != backend->ts_idle && now >= backend->ts_idle) {
		liBackendConnection *bcon;
		for ( ; NULL != (bcon = g_queue_peek_head(&backend->idle_queue)) && now >= bcon->ts_idle; ) {
			backend_con_free(backend, bcon);
		}
		backend->ts_idle = (NULL != bcon) ? bcon->ts_idle : 0;
	}

	if (0 != backend->ts_vr_wait && now >= backend->ts_vr_wait) {
		liBackendWait *bwait;
		for ( ; NULL != (bwait = g_queue_peek_head(&backend->vr_wait_queue)) && now >= bwait->timeout; ) {
			li_job_async(bwait->vr_ref);
			li_job_ref_release(bwait->vr_ref);
			bwait->vr_ref = NULL;
			g_queue_unlink(&backend->vr_wait_queue, &bwait->link);
		}
		backend->ts_vr_wait = (NULL != bwait) ? bwait->timeout : 0;
	}

	backend_master_recalc_timeout(backend);
	g_mutex_unlock(backend->mutex);
}

static void backend_connect_cb(struct ev_loop *loop, struct ev_io *w, int revents) {
	liBackend *backend = w->data;
	struct sockaddr addr;
	socklen_t len;
	int fd = backend->cur_connect_fd;
	ev_tstamp cts = backend->ts_connect;
	UNUSED(loop);
	UNUSED(revents);

	g_mutex_lock(backend->mutex);

	/* stop watching */
	backend->ts_connect = 0;
	backend->cur_connect_fd = -1;
	ev_io_stop(backend->loop, &backend->cur_connect_watcher);
	backend->cur_connect_watcher.fd = -1;

	/* create new connection:
	 * see http://www.cyberconf.org/~cynbe/ref/nonblocking-connects.html
	 */

	/* Check to see if we can determine our peer's address. */
	len = sizeof(addr);
	if (getpeername(fd, &addr, &len) == -1) {
		/* connect failed; find out why */
		int err;
		len = sizeof(err);
#ifdef SO_ERROR
		getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*)&err, &len);
#else
		{
			char ch;
			errno = 0;
			read(fd, &ch, 1);
			err = errno;
		}
#endif
		ERROR(backend->wrk->srv, "Couldn't connect to '%s': %s",
			li_sockaddr_to_string(backend->sock_addr, backend->wrk->tmp_str, TRUE)->str,
			g_strerror(err));

		backend->ts_disabled = cts - backend->connect_timeout + backend->disable_time; /* disable time is relative to original connect start */
		close(fd);
	} else {
		/* connect succeeded */
		backend_con_new(backend, fd, FALSE);
		backend_wakeup_vr(backend); /* new idle slot available */
		backend_wakeup_vr(backend); /* wake up a second one - the connect slot is available too */
	}

	backend_master_recalc_timeout(backend);
	g_mutex_unlock(backend->mutex);
}

static void backend_con_io_cb(struct ev_loop *loop, struct ev_io *w, int revents) {
	liBackendConnection *bcon = w->data;
	liBackend *backend = bcon->backend;
	UNUSED(revents);

	g_mutex_lock(backend->mutex);

	if (bcon->active) {
		ev_io_stop(loop, w);
	} else {
		/* close connection; either unexpected data or EOF or error */
		backend_con_free(backend, bcon);
		backend_wakeup_vr(backend); /* new slot available */
	}

	g_mutex_unlock(backend->mutex);
}

liBackend* li_backend_new(liWorker *wrk, liSocketAddress sock_addr, gint max_connections, guint idle_timeout, guint connect_timeout, guint wait_timeout, guint disable_time, int max_requests) {
	liBackend *backend = g_slice_new0(liBackend);
	backend->wrk = wrk;
	backend->loop = wrk->loop;

	backend->mutex = g_mutex_new();

	backend->sock_addr = li_sockaddr_dup(sock_addr);
	backend->max_connections = max_connections;
	backend->idle_timeout = idle_timeout;
	backend->connect_timeout = connect_timeout;
	backend->wait_timeout = wait_timeout;
	backend->disable_time = disable_time;
	backend->max_requests = max_requests;

	backend->active_connections = g_ptr_array_new();
	backend->update_backends = g_ptr_array_new();
	backend->close_connections = g_ptr_array_new();

	backend->ts_disabled = backend->ts_vr_wait = backend->ts_idle = backend->ts_connect = backend->ts_timeout = 0;

	ev_async_init(&backend->update_watcher, backend_update_cb);
	backend->update_watcher.data = backend;
	ev_async_start(backend->loop, &backend->update_watcher);

	ev_periodic_init(&backend->timeout_watcher, backend_timeout_cb, 0, 0, 0);
	backend->timeout_watcher.data = backend;

	backend->cur_connect_fd = -1;
	ev_io_init(&backend->cur_connect_watcher, backend_connect_cb, -1, EV_READ | EV_WRITE);
	backend->cur_connect_watcher.data = backend;

	return backend;
}

void li_backend_free(liBackend *backend) {
	if (!backend) return;
	g_mutex_lock(backend->mutex);
	backend->shutdown = TRUE;
	ev_async_send(backend->loop, &backend->update_watcher);
	g_mutex_unlock(backend->mutex);
}

/* mutex has to be already locked! */
static void backend_free_wait(liBackend *backend, liBackendWait *bwait) {
	if (bwait->vr_ref) {
		/* in the vr_wait_queue */
		li_job_ref_release(bwait->vr_ref);
		bwait->vr_ref = NULL;

		if (NULL == bwait->link.prev) {
			liBackendWait *next;
			/* first item in the queue */
			g_queue_unlink(&backend->vr_wait_queue, &bwait->link);
			next = g_queue_peek_head(&backend->vr_wait_queue);
			if (NULL == next) {
				backend->ts_vr_wait = 0;
			} else {
				backend->ts_vr_wait = next->timeout;
			}
			backend_recalc_timeout(backend);
		} else {
			/* not the first item, but still in the queue */
			g_queue_unlink(&backend->vr_wait_queue, &bwait->link);
		}
	} else {
		/* not in the queue; so we already got signaled */
		/* signal another vr instead */
		backend_wakeup_vr(backend);
	}

	g_slice_free(liBackendWait, bwait);
}

liBackendResult li_backend_get(liVRequest *vr, liBackend *backend, liBackendConnection **pbcon, liBackendWait **pbwait) {
	liBackendResult result = LI_BACKEND_TIMEOUT;
	liBackendWait *bwait = *pbwait;
	int fd;

	g_mutex_lock(backend->mutex);

	if (backend->shutdown || (NULL != bwait && bwait->timeout <= CUR_TS(vr->wrk))) goto done;

	if (backend->idle_queue.length > 0) {
		GList *elem = g_queue_pop_tail_link(&backend->idle_queue);
		liBackendConnection *bcon = elem->data;

		bcon->active = TRUE;
		bcon->ndx = backend->active_connections->len;
		g_ptr_array_add(backend->active_connections, bcon);

		*pbcon = bcon;
		goto foundcon;
	}

	if (-1 != backend->cur_connect_fd /* already trying to establish a new connection */
	 || backend->ts_disabled >= CUR_TS(vr->wrk) /* connect disabled right now as last connect failed */
	 || (backend->max_connections != -1 && backend->max_connections <= (int) (backend->active_connections->len + backend->idle_queue.length))) { /* no slot available */
		goto wait;
	}

	/* create new connection:
	 * see http://www.cyberconf.org/~cynbe/ref/nonblocking-connects.html
	 */
	do {
		fd = socket(backend->sock_addr.addr->plain.sa_family, SOCK_STREAM, 0);
	} while (-1 == fd && errno == EINTR);
	if (-1 == fd) {
		if (errno == EMFILE) {
			li_server_out_of_fds(vr->wrk->srv);
		}
		VR_ERROR(vr, "Couldn't open socket: %s", g_strerror(errno));
		goto done;
	}
	li_fd_init(fd);

	if (-1 == connect(fd, &backend->sock_addr.addr->plain, backend->sock_addr.len)) {
		switch (errno) {
		case EINPROGRESS:
		case EALREADY:
		case EINTR:
			backend->cur_connect_fd = fd;
			ev_async_send(backend->loop, &backend->update_watcher);
			goto wait;
		case EAGAIN: /* backend overloaded */
			close(fd);
			goto wait;
		case EISCONN:
			break; /* successfully connected */
		default:
			VR_ERROR(vr, "Couldn't connect to '%s': %s",
				li_sockaddr_to_string(backend->sock_addr, vr->wrk->tmp_str, TRUE)->str,
				g_strerror(errno));
			close(fd);
			goto wait;
		}
	}

	*pbcon = backend_con_new(backend, fd, TRUE);
	goto foundcon;

foundcon:
	result = LI_BACKEND_SUCCESS;
	if (bwait) {
		backend_free_wait(backend, bwait);
		*pbwait = NULL;
	}
	goto out;

wait:
	result = LI_BACKEND_WAIT;
	if (!bwait) {
		bwait = g_slice_new0(liBackendWait);
		bwait->link.data = bwait;
		bwait->timeout = CUR_TS(vr->wrk) + backend->wait_timeout;
		bwait->vr_ref = li_vrequest_get_ref(vr);
		if (0 == backend->vr_wait_queue.length) {
			backend->ts_vr_wait = bwait->timeout;
			backend_recalc_timeout(backend);
		}
		g_queue_push_tail_link(&backend->vr_wait_queue, &bwait->link);

		*pbwait = bwait;
	} else {
		if (!bwait->vr_ref) {
			bwait->vr_ref = li_vrequest_get_ref(vr);
			/* insert sorted by timeout */
			if (0 == backend->ts_vr_wait || bwait->timeout <= backend->ts_vr_wait) {
				/* push as first element, update timer */
				g_queue_push_head_link(&backend->vr_wait_queue, &bwait->link);
				backend->ts_vr_wait = bwait->timeout;
				backend_recalc_timeout(backend);
			} else {
				/* not the first element, no need to update timer */
				GList *elem = backend->vr_wait_queue.head, *next;
				liBackendWait *w;
				while ( NULL != (next = elem->next) && bwait->timeout > (w = next->data)->timeout ) {
					elem = next;
				}
				bwait->link.prev = elem; bwait->link.next = next;
				elem->next = &bwait->link;
				if (next) next->prev = &bwait->link;
			}
		}
	}
	goto out;

done:
	if (bwait) {
		backend_free_wait(backend, bwait);
		*pbwait = NULL;
	}
	goto out;

out:
	g_mutex_unlock(backend->mutex);
	return result;
}

void li_backend_wait_stop(liBackend *backend, liBackendWait **pbwait) {
	liBackendWait *bwait = *pbwait;
	if (!bwait) return;

	*pbwait = NULL;
	g_mutex_lock(backend->mutex);

	backend_free_wait(backend, bwait);

	g_mutex_unlock(backend->mutex);
}

void li_backend_put(liBackendConnection *bcon, gboolean closecon) { /* if closecon == TRUE or bcon->fd == -1 the connection gets removed */
	liBackend *backend = bcon->backend;
	g_mutex_lock(backend->mutex);

	bcon->requests++;
	bcon->active = 0;

	closecon = closecon || (-1 == bcon->fd) || backend->shutdown || (-1 != backend->max_requests && bcon->requests >= backend->max_requests);

	/* remove from active connections */
	((liBackendConnection*) g_ptr_array_index(backend->active_connections, bcon->ndx))->ndx = bcon->ndx;
	g_ptr_array_remove_index_fast(backend->active_connections, bcon->ndx);
	bcon->ndx = -1;

	if (closecon) {
		g_ptr_array_add(backend->close_connections, bcon);
		bcon->closing = TRUE;
	} else {
		bcon->ndx = -1;

		/* add to idle queue */
		bcon->ts_idle = ev_time() + backend->idle_timeout;
		if (backend->idle_queue.length == 0) {
			backend->ts_idle = bcon->ts_idle;
			backend_recalc_timeout(backend);
		}
		g_queue_push_tail_link(&backend->idle_queue, &bcon->idle_link);
	}

	ev_async_send(backend->loop, &backend->update_watcher);

	g_mutex_unlock(backend->mutex);
}
