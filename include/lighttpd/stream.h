#ifndef _LIGHTTPD_STREAM_H_
#define _LIGHTTPD_STREAM_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

#include <lighttpd/jobqueue.h>

typedef void (*liStreamCB)(liStream *stream, liStreamEvent event);

struct liStream {
	gint refcount;

	liStream *source, *dest;

	liChunkQueue *out;

	liJob new_data_job;
	liJobQueue *jobqueue;

	liStreamCB cb;
};

LI_API void li_stream_init(liStream* stream, liJobQueue *jobqueue, liStreamCB cb);
LI_API void li_stream_acquire(liStream* stream);
LI_API void li_stream_release(liStream* stream);

LI_API void li_stream_connect(liStream *source, liStream *dest);
LI_API void li_stream_disconnect(liStream *stream); /* disconnects stream->source and stream */
LI_API void li_stream_disconnect_dest(liStream *stream); /* disconnects stream->dest and stream. only for errors/conection resets */
LI_API void li_stream_reset(liStream *stream); /* disconnect both sides */

LI_API void li_stream_notify(liStream *stream); /* new data in stream->cq, notify stream->dest */
LI_API void li_stream_notify_later(liStream *stream);
LI_API void li_stream_again(liStream *stream); /* more data to be generated in stream with event NEW_DATA or more data to be read from stream->source->cq */
LI_API void li_stream_again_later(liStream *stream);

/* detach from jobqueue, stops all event handling. you have to detach all connected streams to move streams between threads */
LI_API void li_stream_detach(liStream *stream);
LI_API void li_stream_attach(liStream *stream, liJobQueue *jobqueue); /* attach to another jobqueue - possibly after switching threads */

/* walks from first using ->dest until it reaches NULL or (it reached last and NULL != i->limit) or limit == i->cq->limit and
 * sets i->cq->limit to limit, triggering LI_STREAM_NEW_CQLIMIT.
 *   limit must not be NULL!
 */
LI_API void li_stream_set_cqlimit(liStream *first, liStream *last, liCQLimit *limit);


LI_API liStream* li_stream_plug_new(liJobQueue *jobqueue); /* simple forwarder; can also be used for providing data from memory */
LI_API liStream* li_stream_null_new(liJobQueue *jobqueue); /* eats everything, disconnects source on eof, out is always closed */



typedef void (*liIOStreamCB)(liIOStream *stream, liIOStreamEvent event);

/* TODO: support throttle */
struct liIOStream {
	liStream stream_in, stream_out;
	liCQLimit *stream_in_limit;

	struct ev_loop *loop;
	ev_io io_watcher;

	/* whether we received EOF or did shutdown(SHUT_WR) */
	gboolean in_closed, out_closed;
	gboolean can_read, can_write; /* set to FALSE if you got EAGAIN */

	liIOStreamCB cb;

	gpointer data; /* data for the callback */
};

LI_API liIOStream* li_iostream_new(liJobQueue *jobqueue, struct ev_loop *loop, int fd, liIOStreamCB cb, gpointer data);
LI_API void li_iostream_acquire(liIOStream* iostream);
LI_API void li_iostream_release(liIOStream* iostream);

LI_API int li_iostream_reset(liIOStream *iostream); /* returns fd, disconnects everything, stop callbacks, releases one reference */

/* similar to stream_detach/_attach */
LI_API void li_iostream_detach(liIOStream *iostream);
LI_API void li_iostream_attach(liIOStream *iostream, liJobQueue *jobqueue, struct ev_loop *loop);

#endif
