#ifndef _LIGHTTPD_FILTER_H_
#define _LIGHTTPD_FILTER_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

typedef liHandlerResult (*liFilterHandlerCB)(liVRequest *vr, liFilter *f);
typedef void (*liFilterFreeCB)(liVRequest *vr, liFilter *f);

struct liFilter {
	liStream *stream;

	liChunkQueue *in, *out;
	liFilterHandlerCB handle_data;
	liFilterFreeCB handle_free;
	gpointer param;

	liVRequest *vr;
	guint filter_ndx;
};

LI_API liFilter* li_filter_new(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, gpointer param);
LI_API void li_filter_free(liFilter *f); /* only closes the stream, doesn't reset it */

LI_API liFilter* li_vrequest_add_filter_in(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, gpointer param);
LI_API liFilter* li_vrequest_add_filter_out(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, gpointer param);

LI_API void li_vrequest_filters_init(liVRequest *vr);
LI_API void li_vrequest_filters_clean(liVRequest *vr);
LI_API void li_vrequest_filters_reset(liVRequest *vr);

#endif
