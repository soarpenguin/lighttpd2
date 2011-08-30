
#include <lighttpd/config_utils.h>

void li_kvlist_init(liKvlistContext* context, const liKvlistEntry* entries, liValue* source) {
	assert(LI_VALUE_HASH == source->type || LI_VALUE_LIST == source->type);
	
	context->source = source;
	context->entries = entries;

	if (LI_VALUE_HASH == source->type) {
		g_hash_table_iter_init(&context->iter.hti, source->data.hash);
	} else { /* (LI_VALUE_LIST == source->type) */
		context->iter.idx = 0;
	}
}

gboolean li_kvlist_next(liKvlistContext* context, guint* idx, liValue** value, GString* error) {
	const liKvlistEntry* entries = context->entries;
	guint i;

	g_string_truncate(error, 0);
	*idx = 0;
	*value = NULL;
	
	if (LI_VALUE_HASH == context->source->type) {
		gpointer pkey, pvalue;
		GString *key;

		if (!g_hash_table_iter_next(&context->iter.hti, &pkey, &pvalue)) {
			return FALSE;
		}
		key = pkey;

		for (i = 0; NULL != entries[i].name; i++) {
			if (key->len == entries[i].len && g_str_equal(key->str, entries[i].name)) {
				*idx = i;
				*value = pvalue;
				return TRUE;
			}
		}

		g_string_printf(error, "Unknown entry '%s' in hash table", key->str);
		*idx = -1;
		return FALSE;
	} else { /* (LI_VALUE_LIST == source->type) */
		GArray *list = context->source->data.list;
		liValue *vpair, *vkey;
		GString *key;

		if (context->iter.idx >= list->len) {
			return FALSE;
		}

		vpair = g_array_index(list, liValue*, context->iter.idx);
		if (NULL == vpair || LI_VALUE_LIST != vpair->type || 2 != vpair->data.list->len) {
			g_string_printf(error, "Expected key-value pair in list");
			*idx = -1;
			return FALSE;
		}

		vkey = g_array_index(vpair->data.list, liValue*, 0);
		if (NULL == vkey || LI_VALUE_STRING != vkey->type) {
			g_string_printf(error, "Expected string as first entry in key-value pair");
			*idx = -1;
			return FALSE;
		}
		key = vkey->data.string;

		for (i = 0; NULL != entries[i].name; i++) {
			if (key->len == entries[i].len && g_str_equal(key->str, entries[i].name)) {
				*idx = i;
				*value = g_array_index(vpair->data.list, liValue*, 1);
				context->iter.idx++;
				return TRUE;
			}
		}

		g_string_printf(error, "Unknown entry '%s' in hash table", key->str);
		*idx = -1;
		return FALSE;
	}
}

void li_kvlist_clear(liKvlistContext *context) {
	memset(context, 0, sizeof(*context));
}
