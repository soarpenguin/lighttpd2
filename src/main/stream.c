
#include <lighttpd/base.h>

/* callback can assume that the stream is not destroyed while the callback is running */
static void li_stream_safe_cb(liStream *stream, liStreamEvent event) {
	if (NULL != stream->cb) {
		li_stream_acquire(stream);
		stream->cb(stream, event);
		li_stream_release(stream);
	}
}

static void stream_new_data_job_cb(liJob *job) {
	liStream *stream = LI_CONTAINER_OF(job, liStream, new_data_job);
	li_stream_safe_cb(stream, LI_STREAM_NEW_DATA);
}

void li_stream_init(liStream* stream, liJobQueue *jobqueue, liStreamCB cb) {
	stream->refcount = 1;
	stream->source = stream->dest = NULL;
	stream->out = li_chunkqueue_new();
	li_job_init(&stream->new_data_job, stream_new_data_job_cb);
	stream->jobqueue = jobqueue;
	stream->cb = cb;
}

void li_stream_acquire(liStream* stream) {
	assert(g_atomic_int_get(&stream->refcount) > 0);
	g_atomic_int_inc(&stream->refcount);
}

void li_stream_release(liStream* stream) {
	assert(g_atomic_int_get(&stream->refcount) > 0);
	if (g_atomic_int_dec_and_test(&stream->refcount)) {
		li_job_clear(&stream->new_data_job);
		li_chunkqueue_free(stream->out);
		stream->out = NULL;
		stream->jobqueue = NULL;
		if (NULL != stream->cb) stream->cb(stream, LI_STREAM_DESTROY); /* "unsafe" cb, we can't keep a ref this time */
	}
}

void li_stream_connect(liStream *source, liStream *dest) {
	/* streams must be "valid" */
	assert(source->refcount > 0 && dest->refcount > 0);

	if (NULL != source->dest || NULL != dest->source) {
		g_error("Can't connect already connected streams");
	}

	/* keep them alive for this function and for callbacks (-> callbacks are "safe") */
	g_atomic_int_inc(&source->refcount);
	g_atomic_int_inc(&dest->refcount);

	/* references for the links */
	g_atomic_int_inc(&source->refcount);
	g_atomic_int_inc(&dest->refcount);
	source->dest = dest;
	dest->source = source;

	if (NULL != source->cb) source->cb(source, LI_STREAM_CONNECTED_DEST);
	/* only notify dest if source didn't disconnect */
	if (source->dest == dest && NULL != dest->cb) dest->cb(dest, LI_STREAM_CONNECTED_SOURCE);

	/* still connected: sync liCQLimit */
	if (source->dest == dest) {
		liCQLimit *sl = source->out->limit, *dl = dest->out->limit;
		if (sl != NULL && dl == NULL) {
			li_stream_set_cqlimit(dest, NULL, sl);
		}
		else if (sl == NULL && dl != NULL) {
			li_stream_set_cqlimit(NULL, source, dl);
		}
	}

	/* still connected and source has data: notify dest */
	if (source->dest == dest && (source->out->length > 0 || source->out->is_closed)) {
		li_stream_again_later(dest);
	}

	/* release our "function" refs */
	li_stream_release(source);
	li_stream_release(dest);
}

static void _disconnect(liStream *source, liStream *dest) {
	/* streams must be "valid" */
	assert(g_atomic_int_get(&source->refcount) > 0 && g_atomic_int_get(&dest->refcount) > 0);
	assert(source->dest == dest && dest->source == source);

	source->dest = NULL;
	dest->source = NULL;
	/* we still have the references from the links -> callbacks are "safe" */
	if (NULL != source->cb) source->cb(source, LI_STREAM_DISCONNECTED_DEST);
	if (NULL != dest->cb) dest->cb(dest, LI_STREAM_DISCONNECTED_SOURCE);

	/* release references from the link */
	li_stream_release(source);
	li_stream_release(dest);
}

void li_stream_disconnect(liStream *stream) {
	if (NULL == stream || NULL == stream->source) return;
	_disconnect(stream->source, stream);
}

void li_stream_disconnect_dest(liStream *stream) {
	if (NULL == stream || NULL == stream->dest) return;
	_disconnect(stream, stream->dest);
}

void li_stream_reset(liStream *stream) {
	if (NULL == stream || 0 == stream->refcount) return;

	li_stream_acquire(stream);
	if (NULL != stream->source) _disconnect(stream->source, stream);
	if (NULL != stream->dest) _disconnect(stream, stream->dest);
	li_stream_release(stream);
}

void li_stream_notify(liStream *stream) {
	if (NULL != stream->dest) li_stream_again(stream->dest);
}

void li_stream_notify_later(liStream *stream) {
	if (NULL != stream->dest) li_stream_again_later(stream->dest);
}

void li_stream_again(liStream *stream) {
	if (NULL != stream->jobqueue) {
		li_job_now(stream->jobqueue, &stream->new_data_job);
	}
}

void li_stream_again_later(liStream *stream) {
	if (NULL != stream->jobqueue) {
		li_job_later(stream->jobqueue, &stream->new_data_job);
	}
}

void li_stream_detach(liStream *stream) {
	stream->jobqueue = NULL;
	li_job_stop(&stream->new_data_job);

	li_chunkqueue_set_limit(stream->out, NULL);
}

void li_stream_attach(liStream *stream, liJobQueue *jobqueue) {
	stream->jobqueue = jobqueue;
	li_stream_again_later(stream);
}

void li_stream_set_cqlimit(liStream *first, liStream *last, liCQLimit *limit) {
	assert(NULL != limit);

	li_cqlimit_acquire(limit);
	if (NULL == first) {
		while (NULL != last && NULL == last->out->limit) {
			if (limit == last->out->limit) break;
			li_chunkqueue_set_limit(last->out, limit);
			if (NULL != last->cb) {
				li_stream_acquire(last);
				last->cb(last, LI_STREAM_NEW_CQLIMIT);
				if (NULL != last->source) {
					last = last->source;
					li_stream_release(last->dest);
				} else {
					li_stream_release(last);
					last = NULL;
				}
			} else {
				last = last->source;
			}
		}
	} else {
		gboolean reached_last = (NULL == last);
		while (NULL != first && !(reached_last && NULL != first->out->limit)) {
			if (limit == first->out->limit) break;
			if (first == last) reached_last = TRUE;
			li_chunkqueue_set_limit(first->out, limit);
			if (NULL != first->cb) {
				li_stream_acquire(first);
				first->cb(first, LI_STREAM_NEW_CQLIMIT);
				if (NULL != first->dest) {
					first = first->dest;
					li_stream_release(first->source);
				} else {
					li_stream_release(first);
					first = NULL;
				}
			} else {
				first = first->dest;
			}
		}
	}
	li_cqlimit_release(limit);
}


static void stream_plug_cb(liStream *stream, liStreamEvent event) {
	switch (event) {
	case LI_STREAM_NEW_DATA:
		if (!stream->out->is_closed && NULL != stream->source) {
			li_chunkqueue_steal_all(stream->out, stream->source->out);
			stream->out->is_closed = stream->out->is_closed || stream->source->out->is_closed;
			li_stream_notify_later(stream);
		}
		if (stream->out->is_closed) {
			li_stream_disconnect(stream);
		}
		break;
	case LI_STREAM_DISCONNECTED_DEST:
		li_stream_disconnect(stream);
		break;
	case LI_STREAM_DESTROY:
		g_slice_free(liStream, stream);
		break;
	default:
		break;
	}
}

liStream* li_stream_plug_new(liJobQueue *jobqueue) {
	liStream *stream = g_slice_new0(liStream);
	li_stream_init(stream, jobqueue, stream_plug_cb);
	return stream;
}

static void stream_null_cb(liStream *stream, liStreamEvent event) {
	switch (event) {
	case LI_STREAM_NEW_DATA:
		if (NULL == stream->source) return;
		li_chunkqueue_skip_all(stream->source->out);
		if (stream->source->out->is_closed) li_stream_disconnect(stream);
		break;
	case LI_STREAM_DESTROY:
		g_slice_free(liStream, stream);
		break;
	default:
		break;
	}
}

liStream* li_stream_null_new(liJobQueue *jobqueue) {
	liStream *stream = g_slice_new0(liStream);
	li_stream_init(stream, jobqueue, stream_null_cb);
	stream->out->is_closed = TRUE;
	return stream;
}


static void iostream_destroy(liIOStream *iostream) {
	if (0 < iostream->stream_out.refcount && 0 < iostream->stream_in.refcount) return;

	if (NULL != iostream->stream_in_limit) {
		if (&iostream->io_watcher == iostream->stream_in_limit->io_watcher) {
			iostream->stream_in_limit->io_watcher = NULL;
		}
		li_cqlimit_release(iostream->stream_in_limit);
		iostream->stream_in_limit = NULL;
	}

	ev_io_stop(iostream->loop, &iostream->io_watcher);

	iostream->cb(iostream, LI_IOSTREAM_DESTROY);
}

static void iostream_in_cb(liStream *stream, liStreamEvent event) {
	liIOStream *iostream = LI_CONTAINER_OF(stream, liIOStream, stream_in);

	switch (event) {
	case LI_STREAM_NEW_DATA:
		if (0 == li_chunkqueue_limit_available(stream->out)) {
			/* locked */
			return;
		}
		if (iostream->can_read) {
			goffset curoutlen = stream->out->length;
			gboolean curout_closed = stream->out->is_closed;

			iostream->cb(iostream, LI_IOSTREAM_READ);

			if (curoutlen != stream->out->length || curout_closed != stream->out->is_closed) {
				li_stream_notify_later(stream);
			}

			if (-1 == iostream->io_watcher.fd) return;

			if (iostream->can_read) {
				li_stream_again_later(stream);
			}
		}
		if (!iostream->can_read && !iostream->in_closed) {
			li_ev_io_add_events(iostream->loop, &iostream->io_watcher, EV_READ);
		}
		if (!iostream->can_write && !iostream->out_closed) {
			li_ev_io_add_events(iostream->loop, &iostream->io_watcher, EV_WRITE);
		}
		break;
	case LI_STREAM_NEW_CQLIMIT:
		if (NULL != iostream->stream_in_limit) {
			if (&iostream->io_watcher == iostream->stream_in_limit->io_watcher) {
				iostream->stream_in_limit->io_watcher = NULL;
			}
			li_cqlimit_release(iostream->stream_in_limit);
		}
		if (stream->out->limit) {
			stream->out->limit->io_watcher = &iostream->io_watcher;
			li_cqlimit_acquire(stream->out->limit);
		}
		iostream->stream_in_limit = stream->out->limit;
		break;
	case LI_STREAM_CONNECTED_SOURCE:
		/* there is no incoming data */
		li_stream_disconnect(stream);
		break;
	case LI_STREAM_CONNECTED_DEST:
		iostream->cb(iostream, LI_IOSTREAM_CONNECTED_DEST);
		break;
	case LI_STREAM_DISCONNECTED_DEST:
		iostream->cb(iostream, LI_IOSTREAM_DISCONNECTED_DEST);
		break;
	case LI_STREAM_DESTROY:
		iostream->can_read = FALSE;
		iostream_destroy(iostream);
		break;
	default:
		break;
	}
}

static void iostream_out_cb(liStream *stream, liStreamEvent event) {
	liIOStream *iostream = LI_CONTAINER_OF(stream, liIOStream, stream_out);

	switch (event) {
	case LI_STREAM_NEW_DATA:
		if (iostream->can_write) {
			iostream->cb(iostream, LI_IOSTREAM_WRITE);

			if (-1 == iostream->io_watcher.fd) return;

			if (iostream->can_write) {
				if (stream->out->length > 0 || stream->out->is_closed) {
					li_stream_again_later(stream);
				}
			}
		}
		if (!iostream->can_read && !iostream->in_closed) {
			li_ev_io_add_events(iostream->loop, &iostream->io_watcher, EV_READ);
		}
		if (!iostream->can_write && !iostream->out_closed) {
			li_ev_io_add_events(iostream->loop, &iostream->io_watcher, EV_WRITE);
		}
		break;
	case LI_STREAM_CONNECTED_DEST:
		/* there is no outgoing data */
		li_stream_disconnect_dest(stream);
		break;
	case LI_STREAM_CONNECTED_SOURCE:
		iostream->cb(iostream, LI_IOSTREAM_CONNECTED_SOURCE);
		break;
	case LI_STREAM_DISCONNECTED_SOURCE:
		iostream->cb(iostream, LI_IOSTREAM_DISCONNECTED_SOURCE);
		break;
	case LI_STREAM_DESTROY:
		iostream->can_write = FALSE;
		iostream_destroy(iostream);
		break;
	default:
		break;
	}
}

static void iostream_io_cb(struct ev_loop *loop, ev_io *w, int revents) {
	liIOStream *iostream = (liIOStream*) w->data;
	gboolean do_write = FALSE;
	UNUSED(loop);

	li_ev_io_rem_events(iostream->loop, &iostream->io_watcher, EV_WRITE | EV_READ);

	if (0 != (revents & EV_WRITE) && !iostream->can_write && iostream->stream_out.refcount > 0) {
		iostream->can_write = TRUE;
		do_write = TRUE;
		li_stream_acquire(&iostream->stream_out); /* keep out stream alive during li_stream_again(&iostream->stream_in) */
	}

	if (0 != (revents & EV_READ) && !iostream->can_read && iostream->stream_in.refcount > 0) {
		iostream->can_read = TRUE;
		li_stream_again_later(&iostream->stream_in);
	}

	if (do_write) {
		li_stream_again_later(&iostream->stream_out);
		li_stream_release(&iostream->stream_out);
	}
}

liIOStream* li_iostream_new(liJobQueue *jobqueue, struct ev_loop *loop, int fd, liIOStreamCB cb, gpointer data) {
	liIOStream *iostream = g_slice_new0(liIOStream);

	li_stream_init(&iostream->stream_in, jobqueue, iostream_in_cb);
	li_stream_init(&iostream->stream_out, jobqueue, iostream_out_cb);
	iostream->stream_in_limit = NULL;

	iostream->loop = loop;
	ev_io_init(&iostream->io_watcher, iostream_io_cb, fd, EV_READ);
	iostream->io_watcher.data = iostream;

	iostream->in_closed = iostream->out_closed = iostream->can_read = FALSE;
	iostream->can_write = TRUE;

	iostream->cb = cb;
	iostream->data = data;

	ev_io_start(iostream->loop, &iostream->io_watcher);

	return iostream;
}

void li_iostream_acquire(liIOStream* iostream) {
	li_stream_acquire(&iostream->stream_in);
	li_stream_acquire(&iostream->stream_out);
}

void li_iostream_release(liIOStream* iostream) {
	li_stream_release(&iostream->stream_in);
	li_stream_release(&iostream->stream_out);
}

int li_iostream_reset(liIOStream *iostream) {
	int fd;
	if (NULL == iostream) return -1;

	fd = iostream->io_watcher.fd;
	if (NULL != iostream->loop) {
		ev_io_stop(iostream->loop, &iostream->io_watcher);
	}
	ev_io_set(&iostream->io_watcher, -1, 0);

	li_stream_disconnect(&iostream->stream_out);
	li_stream_disconnect_dest(&iostream->stream_in);

	li_iostream_release(iostream);

	return fd;
}

void li_iostream_detach(liIOStream *iostream) {
	if (NULL != iostream->loop) {
		ev_io_stop(iostream->loop, &iostream->io_watcher);
		iostream->loop = NULL;
	}

	if (NULL != iostream->stream_in_limit) {
		if (&iostream->io_watcher == iostream->stream_in_limit->io_watcher) {
			iostream->stream_in_limit->io_watcher = NULL;
		}
		li_cqlimit_release(iostream->stream_in_limit);
		iostream->stream_in_limit = NULL;
	}

	li_stream_detach(&iostream->stream_in);
	li_stream_detach(&iostream->stream_out);
}

void li_iostream_attach(liIOStream *iostream, liJobQueue *jobqueue, struct ev_loop *loop) {
	assert(NULL == iostream->loop);

	li_stream_attach(&iostream->stream_in, jobqueue);
	li_stream_attach(&iostream->stream_out, jobqueue);

	iostream->loop = loop;
	ev_io_start(iostream->loop, &iostream->io_watcher);
}
