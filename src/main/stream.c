
#include <lighttpd/base.h>

static void _disconnect(liStreamSource *source, liStreamDrain *drain) {
	assert(source->valid);
	assert(drain->valid);

	source->drain = NULL;
	drain->source = NULL;
	li_stream_source_notify(source);
	li_stream_drain_notify(drain);
}

void li_stream_source_init(liStreamSource *source, liStreamSourceCB handle_wakeup) {
	source->cq = li_chunkqueue_new();
	source->drain = NULL;
	source->handle_wakeup = handle_wakeup;
	source->handle_wakeup_entered = FALSE;
	source->cq_is_closed = FALSE;
	source->cq_bytes_in = 0;
	source->delay_notify = 0;
	source->valid = TRUE;
}
void li_stream_source_disconnect(liStreamSource *source) {
	if (NULL == source || NULL == source->drain) return;
	_disconnect(source, source->drain);
}
void li_stream_source_clear(liStreamSource *source) {
	if (NULL == source || NULL == source->handle_wakeup) return;

	/* prevent further callbacks */
	source->handle_wakeup = NULL;

	if (NULL != source->drain) _disconnect(source, source->drain);

	li_chunkqueue_free(source->cq);
	source->cq = NULL;
	source->cq_is_closed = TRUE;
	source->cq_bytes_in = 0;
	source->valid = FALSE;
}
void li_stream_source_reset(liStreamSource *source) {
	li_chunkqueue_reset(source->cq);
	source->cq_is_closed = FALSE;
	source->cq_bytes_in = 0;
	li_stream_source_notify(source);
}

void li_stream_source_notify(liStreamSource *source) {
	liStreamDrain *drain = source->drain;
	if (!source->valid || NULL == drain || !drain->valid
		|| NULL == drain->handle_wakeup
		|| drain->handle_wakeup_entered
		|| drain->delay_notify > 0) return;

	drain->handle_wakeup_entered = TRUE;
	drain->handle_wakeup(drain);
	drain->handle_wakeup_entered = FALSE;
}
void li_stream_source_notify_pause(liStreamSource *source) {
	g_assert(source->delay_notify < G_MAXUINT-1);
	source->delay_notify++;
}
void li_stream_source_notify_continue(liStreamSource *source) {
	g_assert(source->delay_notify > 0);
	source->delay_notify--;

	if (0 != source->delay_notify) return;
	if (!source->valid) return;

	if (source->cq_is_closed == source->cq->is_closed
		&& source->cq_bytes_in == source->cq->bytes_in)
		return;

	source->cq_is_closed = source->cq->is_closed;
	source->cq_bytes_in = source->cq->bytes_in;

	li_stream_source_notify(source);
}

void li_stream_source_null_cb(liStreamSource *source) {
	UNUSED(source);
}

void li_stream_drain_init(liStreamDrain *drain, liStreamDrainCB handle_wakeup) {
	drain->want_data = TRUE;
	drain->source = NULL;
	drain->handle_wakeup = handle_wakeup;
	drain->handle_wakeup_entered = FALSE;
	drain->delay_notify = 0;
	drain->valid = TRUE;
}
void li_stream_drain_disconnect(liStreamDrain *drain) {
	if (NULL == drain || NULL == drain->source) return;
	_disconnect(drain->source, drain);
}
void li_stream_drain_clear(liStreamDrain *drain) {
	if (NULL == drain || NULL == drain->handle_wakeup) return;

	/* prevent further callbacks */
	drain->handle_wakeup = NULL;

	if (NULL != drain->source) _disconnect(drain->source, drain);

	drain->want_data = FALSE;
	drain->valid = FALSE;
}

void li_stream_drain_notify(liStreamDrain *drain) {
	liStreamSource *source = drain->source;
	if (!drain->valid || NULL == source || !source->valid
		|| NULL == source->handle_wakeup
		|| source->handle_wakeup_entered
		|| source->delay_notify > 0) return;

	source->handle_wakeup_entered = TRUE;
	source->handle_wakeup(source);
	source->handle_wakeup_entered = FALSE;
}
void li_stream_drain_notify_pause(liStreamDrain *drain) {
	g_assert(drain->delay_notify < G_MAXUINT-1);
	drain->delay_notify++;
}
void li_stream_drain_notify_continue(liStreamDrain *drain) {
	g_assert(drain->delay_notify > 0);
	drain->delay_notify--;
	if (0 == drain->delay_notify) {
		li_stream_drain_notify(drain);
	}
}

void li_stream_drain_null_cb(liStreamDrain *drain) {
	liStreamSource *source = drain->source;

	if (NULL != source) {
		li_chunkqueue_skip_all(source->cq);
		li_stream_drain_notify(drain);
	}
}

void li_stream_connect(liStreamSource *source, liStreamDrain *drain) {
	g_assert(source->valid);
	g_assert(drain->valid);

	if (source->drain == drain) {
		/* ignore reconnect with same parameters */
		g_assert(drain->source == source);
		return;
	}

	g_assert(NULL == source->drain);
	g_assert(NULL == drain->source);

	drain->source = source;
	source->drain = drain;

	li_stream_source_notify(source);
	li_stream_drain_notify(drain);
}

static void stream_source_wakeup_cb(liStreamSource *source) {
	liStream *stream = LI_CONTAINER_OF(source, liStream, source);

	if (NULL == source->drain) {
		assert(stream->source_connected);

		if (stream->source_connected) {
			/* got disconnected */
			stream->source_connected = FALSE;
			li_stream_wakeup(stream);
			li_stream_release(stream);
		}
	} else {
		if (!stream->source_connected) {
			/* got connected */
			stream->source_connected = TRUE;
			li_stream_acquire(stream);
		}
		li_stream_wakeup(stream);
	}
}
static void stream_drain_wakeup_cb(liStreamDrain *drain) {
	liStream *stream = LI_CONTAINER_OF(drain, liStream, drain);

	if (NULL == drain->source) {
		assert(stream->drain_connected);

		if (stream->drain_connected) {
			/* got disconnected */
			stream->drain_connected = FALSE;
			li_stream_wakeup(stream);
			li_stream_release(stream);
		}
	} else {
		if (!stream->drain_connected) {
			stream->drain_connected = TRUE;
			li_stream_acquire(stream);
		}

		if (drain->source->cq->length > 0 || drain->source->cq->is_closed) {
			li_stream_wakeup(stream);
		} else {
			li_stream_wakeup_later(stream);
		}
	}
}
static void stream_job_cb(liJob *job) {
	liStream *stream = LI_CONTAINER_OF(job, liStream, job);

	if (NULL == stream->handle_data) return;

	li_stream_acquire(stream);
	li_stream_source_notify_pause(&stream->source);
	li_stream_drain_notify_pause(&stream->drain);

	stream->handle_data(stream);

	{
		/* disconnect source of drain if reached eof */
		liStreamSource *dsource = stream->drain.source;
		if (NULL != dsource && dsource->valid
			&& 0 == dsource->cq->length && dsource->cq->is_closed) {
			li_stream_drain_disconnect(&stream->drain);
		}
	}

	li_stream_drain_notify_continue(&stream->drain);
	li_stream_source_notify_continue(&stream->source);
	li_stream_release(stream);
}

liStream* li_stream_new(liJobQueue* jobqueue, gpointer data, liStreamCB handle_data, liStreamCB handle_free) {
	liStream *stream = g_slice_new0(liStream);
	stream->refcount = 3;
	stream->data = data;

	li_stream_source_init(&stream->source, stream_source_wakeup_cb);
	li_stream_drain_init(&stream->drain, stream_drain_wakeup_cb);

	stream->handle_data = handle_data;
	stream->handle_free = handle_free;

	li_job_init(&stream->job, stream_job_cb);
	stream->jobqueue = jobqueue;

	return stream;
}
static void stream_free(liStream *stream) {
	if (NULL == stream) return;

	/* source and drain are disconnected (they keep a reference while connected) */
	g_assert(!stream->source.drain);
	g_assert(!stream->drain.source);

	li_stream_source_clear(&stream->source);
	li_stream_drain_clear(&stream->drain);

	li_job_clear(&stream->job);
	stream->jobqueue = NULL;

	stream->handle_data = NULL;

	if (NULL != stream->handle_free) {
		stream->handle_free(stream);
	}

	stream->data = NULL;
	stream->handle_free = NULL;
}

void li_stream_acquire(liStream *stream) {
	assert(g_atomic_int_get(&stream->refcount) > 0);
	g_atomic_int_inc(&stream->refcount);
}
void li_stream_release(liStream *stream) {
	if (NULL == stream) return;
	assert(g_atomic_int_get(&stream->refcount) > 0);
	if (g_atomic_int_dec_and_test(&stream->refcount)) {
		stream_free(stream);
	}
}
void li_stream_reset(liStream *stream) {
	/* disable callbacks */
	stream->data = NULL;
	stream->handle_free = NULL;
	stream->handle_data = NULL;

	li_stream_source_disconnect(&stream->source);
	li_stream_drain_disconnect(&stream->drain);

	li_stream_wakeup(stream);
}
void li_stream_close(liStream *stream) {
	/* disable callbacks */
	stream->data = NULL;
	stream->handle_free = NULL;
	stream->handle_data = NULL;

	li_stream_drain_disconnect(&stream->drain);

	stream->source.cq->is_closed = TRUE;
	li_stream_source_notify(&stream->source);
}
void li_stream_wakeup(liStream *stream) {
	if (NULL != stream->jobqueue && NULL != stream->handle_data) {
		li_job_now(stream->jobqueue, &stream->job);
	}
}

void li_stream_wakeup_later(liStream *stream) {
	if (NULL != stream->jobqueue && NULL != stream->handle_data) {
		li_job_later(stream->jobqueue, &stream->job);
	}
}

static void stream_plug_source_wakeup_cb(liStreamSource *source) {
	UNUSED(source);
}
static void stream_plug_drain_wakeup_cb(liStreamDrain *drain) {
	if (NULL != drain->source) {
		liChunkQueue *cq = drain->source->cq;
		li_chunkqueue_skip_all(cq);
		if (cq->is_closed) {
			/* close after eof */
			li_stream_drain_disconnect(drain);
		}
	}
}
void li_stream_plug_init(liStreamPlug *plug) {
	li_stream_source_init(&plug->source, stream_plug_source_wakeup_cb);
	li_stream_drain_init(&plug->drain, stream_plug_drain_wakeup_cb);

	plug->source.cq->is_closed = TRUE;
}
void li_stream_plug_clean(liStreamPlug *plug) {
	li_stream_source_clear(&plug->source);
	li_stream_drain_clear(&plug->drain);
}
void li_stream_plug_reset(liStreamPlug *plug) {
	li_stream_source_disconnect(&plug->source);
	li_stream_drain_disconnect(&plug->drain);
}

static void io_stream_update(liIOStream* stream) {
	int events = 0;

	if (-1 == stream->io_watcher.fd) return;

	if (!stream->in_closed &&
		(!stream->can_read
		|| (0 == stream->source.cq->length) /* we always want something in the buffer to detect eof early */
		|| (NULL != stream->source.drain && stream->source.drain->want_data))
		) events |= EV_READ;

	if (!stream->out_closed &&
		(!stream->can_write
		|| (out->length > 0))
		) events |= EV_WRITE;

	li_ev_io_set_events(stream->loop, &stream->io_watcher, events);
}

static void io_stream_cb(struct ev_loop *loop, ev_io *w, int revents) {
	liIOStream* stream = w->data;
}
static void io_stream_source_wakeup_cb(liStreamSource *source) {
	liIOStream *stream = LI_CONTAINER_OF(source, liIOStream, source);

	io_stream_update(stream);
}
static void io_stream_drain_wakeup_cb(liStreamDrain *drain) {
	liIOStream *stream = LI_CONTAINER_OF(drain, liIOStream, drain);

	if (NULL != stream->drain.source) {
		li_chunkqueue_steal_all(stream->out, stream->drain.source->cq);
		if (stream->drain.source->cq->is_closed) {
			stream->out->is_closed = TRUE;
			li_stream_drain_disconnect(drain);
		}
	}

	io_stream_update(stream);
}

void li_io_stream_init(liIOStream* stream, struct ev_loop *loop, int fd, liIOStreamCB handle_wakeup) {
	li_stream_source_init(stream->source, io_stream_source_wakeup_cb);
	li_stream_drain_init(stream->drain, io_stream_drain_wakeup_cb);

	stream->loop = loop;
	ev_io_init(&stream->io_watcher, io_stream_cb, fd, EV_READ);
	stream->io_watcher.data = stream;
	ev_io_start(stream->loop, &stream->io_watcher);

	stream->can_write = TRUE;
}
void li_io_stream_shutdown(liIOStream* stream) {
}
void li_io_stream_reset(liIOStream* stream) {
}
