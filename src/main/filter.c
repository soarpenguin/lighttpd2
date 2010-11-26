
#include <lighttpd/base.h>
#include <lighttpd/filter.h>

void li_vrequest_filters_init(liVRequest *vr) {
	vr->filters_in_last = vr->filters_out_last = NULL;
	vr->filters_in_first = vr->filters_out_first = NULL;
	vr->filters = g_ptr_array_new();
}

void li_vrequest_filters_clean(liVRequest *vr) {
	while (vr->filters->len > 0) {
		li_filter_free(g_ptr_array_index(vr->filters, vr->filters->len - 1));
	}
	vr->filters_in_last = vr->filters_out_last = NULL;
	vr->filters_in_first = vr->filters_out_first = NULL;
}

void li_vrequest_filters_reset(liVRequest *vr) {
	li_vrequest_filters_clean(vr);
	g_ptr_array_free(vr->filters, TRUE);
	vr->filters = NULL;
}

static void filter_handle_data_cb(liStream *stream) {
	liFilter *f = stream->data;

	if (NULL != f->handle_data) {
		f->handle_data(f->vr, f);
	}
}

liFilter* li_filter_new(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, gpointer param) {
	liFilter *f;

	f = g_slice_new0(liFilter);
	f->param = param;
	f->handle_data = handle_data;
	f->handle_free = handle_free;

	f->stream = li_stream_new(&vr->wrk->jobqueue, f, filter_handle_data_cb, NULL);

	f->vr = vr;
	f->filter_ndx = vr->filters->len;
	g_ptr_array_add(vr->filters, f);

	return f;
}

void li_filter_free(liFilter *f) {
	liVRequest *vr = f->vr;

	if (NULL != f->handle_free) {
		f->handle_free(f->vr, f);
	}

	if (NULL != f->stream) {
		li_stream_close(f->stream);
		li_stream_release(f->stream);
		f->stream = NULL;
	}

	/* remove from vr filters list */
	assert(vr->filters->len > 0);
	if (vr->filters->len - 1 != f->filter_ndx) {
		/* not the last filter, swap: */
		liFilter *last = g_ptr_array_index(vr->filters, vr->filters->len - 1);
		last->filter_ndx = f->filter_ndx;
		g_ptr_array_index(vr->filters, f->filter_ndx) = last;
	}
	g_ptr_array_set_size(vr->filters, vr->filters->len - 1);

	g_slice_free(liFilter, f);
}


liFilter* li_vrequest_add_filter_in(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, gpointer param) {
	liFilter *f;

	/* as soon as we have a backend drain it gets connected -> too late for in filters */
	if (NULL != vr->backend_drain) return NULL;

	f = li_filter_new(vr, handle_data, handle_free, param);
	li_chunkqueue_set_limit(f->stream->source.cq, vr->coninfo->cqlimit_in);

	if (NULL == vr->filters_in_first) {
		assert(NULL == vr->filters_in_last);

		vr->filters_in_first = &f->stream->drain;
		vr->filters_in_last = &f->stream->source;
	} else {
		assert(NULL != vr->filters_in_last);

		li_stream_connect(vr->filters_in_last, &f->stream->drain);
		vr->filters_in_last = &f->stream->source;
	}

	return f;
}

liFilter* li_vrequest_add_filter_out(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, gpointer param) {
	liFilter *f;

	/* if backend source is connected it is too late for our filters */
	if (NULL != vr->backend_source && NULL != vr->backend_source->drain) return NULL;

	f = li_filter_new(vr, handle_data, handle_free, param);
	li_chunkqueue_set_limit(f->stream->source.cq, vr->coninfo->cqlimit_out);

	if (NULL == vr->filters_out_first) {
		assert(NULL == vr->filters_out_last);

		vr->filters_out_first = &f->stream->drain;
		vr->filters_out_last = &f->stream->source;
	} else {
		assert(NULL != vr->filters_out_last);

		li_stream_connect(vr->filters_out_last, &f->stream->drain);
		vr->filters_out_last = &f->stream->source;
	}

	return f;
}
