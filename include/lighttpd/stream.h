#ifndef _LIGHTTPD_STREAM_H_
#define _LIGHTTPD_STREAM_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

#include <lighttpd/jobqueue.h>

typedef void (*liStreamSourceCB)(liStreamSource *source);
typedef void (*liStreamDrainCB)(liStreamDrain *drain);
typedef void (*liStreamCB)(liStream *stream);
typedef void (*liIOStreamCB)(liIOStream *stream);

struct liStreamSource {
	gboolean valid;
	liChunkQueue *cq;

	liStreamDrain *drain;

	/* called on:
	 *  - connect
	 *  - disconnect
	 *  - change of drain->want_data
	 */
	liStreamSourceCB handle_wakeup;
	gboolean handle_wakeup_entered; /* prevent re-enter */

	/* last state */
	gboolean cq_is_closed;
	goffset cq_bytes_in;

	guint delay_notify;
};

struct liStreamDrain {
	gboolean valid;
	gboolean want_data;

	liStreamSource *source;

	/* called on:
	 *  - connect
	 *  - disconnect
	 *  - change of source->cq
	 */
	liStreamDrainCB handle_wakeup;
	gboolean handle_wakeup_entered; /* prevent re-enter */

	guint delay_notify;
};

struct liStream {
	int refcount; /* source and drain keep reference while "connected" */
	gpointer data;

	liStreamSource source;
	liStreamDrain drain;

	gboolean source_connected, drain_connected;

	/* read from drain->source->cq, write to source->cq (you have to check whether source/drain are valid)
	 * drain gets cleared (disconnected) if drain->source->cq is empty and closed after handle_data
	 */
	liStreamCB handle_data;
	liStreamCB handle_free;

	liJob job;
	liJobQueue *jobqueue;
};

/* only forwards data */
struct liStreamPlug {
	liStreamSource source;
	liStreamDrain drain;
};

/* if io_watcher.fd == -1, it was closed after errors
 */
/* TODO */
/* TODO: support throttle */
struct liIOStream {
	struct ev_loop *loop;
	ev_io io_watcher;

	liStreamSource source;
	liStreamDrain drain;

	liChunkQueue *out;

	/* whether we received EOF or did shutdown(SHUT_WR); on errors both get set */
	gboolean in_closed, out_closed;
	gboolean can_read, can_write;

	/* called on:
	 *  - connect
	 *  - disconnect
	 *  - drain disconnected and out gets empty
	 *  - out is closed and gets empty
	 */
	liIOStreamCB handle_wakeup;
	gboolean handle_wakeup_entered; /* prevent re-enter */

	/* I/O timeout data */
	liWaitQueueElem io_timeout_elem;
};

/* in normal cases the drain is resonsible to disconnect after it read the "eof" (source->cq empty and closed);
 * don't clear/disconnect the source as long as a drain is connected (unless there was an error/...)
 */
LI_API void li_stream_source_init(liStreamSource *source, liStreamSourceCB handle_wakeup);
LI_API void li_stream_source_disconnect(liStreamSource *source);
LI_API void li_stream_source_clear(liStreamSource *source); /* doesn't trigger callbacks on source */

/* reset cq */
LI_API void li_stream_source_reset(liStreamSource *source);

LI_API void li_stream_source_notify(liStreamSource *source); /* call after changing source->cq, notifies connected drain */
LI_API void li_stream_source_notify_pause(liStreamSource *source);
LI_API void li_stream_source_notify_continue(liStreamSource *source);

LI_API void li_stream_source_null_cb(liStreamSource *source); /* callback for li_stream_source_init to do nothing */

LI_API void li_stream_drain_init(liStreamDrain *drain, liStreamDrainCB handle_wakeup);
LI_API void li_stream_drain_disconnect(liStreamDrain *drain);
LI_API void li_stream_drain_clear(liStreamDrain *drain); /* doesn't trigger callbacks on drain */

LI_API void li_stream_drain_notify(liStreamDrain *drain); /* call after reading from drain->source->cq, notifies connected source */
LI_API void li_stream_drain_notify_pause(liStreamDrain *drain);
LI_API void li_stream_drain_notify_continue(liStreamDrain *drain);

LI_API void li_stream_drain_null_cb(liStreamDrain *drain); /* callback for li_stream_drain_init to drop all data */

LI_API void li_stream_connect(liStreamSource *source, liStreamDrain *drain);

LI_API liStream* li_stream_new(liJobQueue *jobqueue, gpointer data, liStreamCB handle_data, liStreamCB handle_free);
LI_API void li_stream_acquire(liStream *stream);
LI_API void li_stream_release(liStream *stream);

/* disconnects both ends and resets them, disables handle_data/handle_free callback;
 * call this on errors, otherwise use li_stream_close
 */
LI_API void li_stream_reset(liStream *stream);

/* disconnects and resets drain, closes the source->cq queue, disables handle_data/handle_free callback;
 * this will keep the stream alive until the next drain is done reading from our source
 */
LI_API void li_stream_close(liStream *stream);
LI_API void li_stream_wakeup(liStream *stream);
LI_API void li_stream_wakeup_later(liStream *stream);

LI_API void li_stream_plug_init(liStreamPlug *plug);
LI_API void li_stream_plug_clean(liStreamPlug *plug);
LI_API void li_stream_plug_reset(liStreamPlug *plug);

LI_API void li_io_stream_init(liIOStream* stream, struct ev_loop *loop, int fd, liIOStreamCB handle_wakeup);
LI_API void li_io_stream_shutdown(liIOStream* stream); /* shutdown(SHUT_WR) + out_closed = true */
LI_API void li_io_stream_reset(liIOStream* stream);

#endif
