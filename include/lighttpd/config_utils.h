#ifndef _LIGHTTPD_CONFIG_UTILS_H_
#define _LIGHTTPD_CONFIG_UTILS_H_

#include <lighttpd/base.h>

typedef struct liKvlistEntry liKvlistEntry;
typedef struct liKvlistContext liKvlistContext;

struct liKvlistEntry {
	const char *name;
	guint len;
};

struct liKvlistContext {
	liValue *source;
	const liKvlistEntry *entries;

	union {
		GHashTableIter hti;
		guint idx;
	} iter;
};

/* needs LIST or HASH */
LI_API void li_kvlist_init(liKvlistContext *context, const liKvlistEntry *entries, liValue *source);
LI_API gboolean li_kvlist_next(liKvlistContext *context, guint *idx, liValue **value, GString *error);
LI_API void li_kvlist_clear(liKvlistContext *context);

#endif