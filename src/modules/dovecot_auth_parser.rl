
#include <lighttpd/base.h>
#include <lighttpd/dovecot_auth_parser.h>

/** Machine **/

#define _getString(M, FPC) (li_chunk_extract(vr, ctx->M, GETMARK(FPC)))
#define getString(FPC) _getString(mark, FPC)

#define _getStringTo(M, FPC, s) (li_chunk_extract_to(vr, ctx->M, GETMARK(FPC), s))
#define getStringTo(FPC, s) _getStringTo(mark, FPC, s)


%%{

	machine li_dovecot_auth_parser;
	variable cs ctx->chunk_ctx.cs;

	action mark { ctx->mark = GETMARK(fpc); }

	action method { getStringTo(fpc, ctx->request->http_method_str); }
	action uri { getStringTo(fpc, ctx->request->uri.raw); }

	action version_error {
	}

	action start_id {
		id = 0;
	}
	action parse_id {
		if ( (G_MAXUINT32 - (fc - '0')) / 10 > id) {
			/* overflow */
			fgoto *li_dovecot_auth_parser_error;
			fbreak;
		}
		id = 10 * id + (fc - '0');
	}

	action mech_plain {
	}
	action mech_login {
	}

	action server_done {
	}

	action auth_fail_start {
	}
	action auth_fail_reason {
	}
	action auth_fail_temp {
	}
	action auth_fail_authz {
	}
	action auth_fail {
	}
	action auth_cont {
	}
	action auth_ok {
	}

	optparams = ("\t" print+)*;

	version = "VERSION\t" ("1\t" digit+) $err(version_error);
	spid = "SPID\t" (print+);
	cuid = "CUID\t" (print+);
	cookie = "COOKIE\t" (xdigit+); # ignore length - should be 128 xdigits
	mech = "MECH\t" ("PLAIN" %mech_plain | "LOGIN" %mech_login | print+);
	done = "DONE" %server_done;

	id = digit+ >start_id $parse_id;

	fail = ("FAIL\t" @auth_fail_start id ("\t" ("reason=" (print+)>mark %auth_fail_reason | "temp" %auth_fail_temp | "authz" %auth_fail_authz | print+))*) %auth_fail;
	cont = "CONT\t" id "\t" (print+) %auth_cont;
	ok = "OK\t" id %auth_ok;

	main := ((version | spid | cuid | cookie | mech | done | fail | cont | ok) optparams "\n")*;
}%%

%% write data;

static int li_dovecot_auth_parser_has_error(liHttpRequestCtx *ctx) {
	return ctx->chunk_ctx.cs == li_dovecot_auth_parser_error;
}

static int li_dovecot_auth_parser_is_finished(liHttpRequestCtx *ctx) {
	return ctx->chunk_ctx.cs >= li_dovecot_auth_parser_first_final;
}

void li_dovecot_auth_parser_init(liHttpRequestCtx* ctx, liRequest *req, liChunkQueue *cq) {
	li_chunk_parser_init(&ctx->chunk_ctx, cq);
	ctx->request = req;
	ctx->h_key = g_string_sized_new(0);
	ctx->h_value = g_string_sized_new(0);

	%% write init;
}

void li_dovecot_auth_parser_reset(liHttpRequestCtx* ctx) {
	li_chunk_parser_reset(&ctx->chunk_ctx);
	g_string_truncate(ctx->h_key, 0);
	g_string_truncate(ctx->h_value, 0);

	%% write init;
}

void li_dovecot_auth_parser_clear(liHttpRequestCtx *ctx) {
	g_string_free(ctx->h_key, TRUE);
	g_string_free(ctx->h_value, TRUE);
}

liHandlerResult li_dovecot_auth_parse(liVRequest *vr, liHttpRequestCtx *ctx) {
	liHandlerResult res;
	guint id; /* move to context */

	if (li_dovecot_auth_parser_is_finished(ctx)) return LI_HANDLER_GO_ON;

	if (LI_HANDLER_GO_ON != (res = li_chunk_parser_prepare(&ctx->chunk_ctx))) return res;

	while (!li_dovecot_auth_parser_has_error(ctx) && !li_dovecot_auth_parser_is_finished(ctx)) {
		char *p, *pe, *eof = 0;

		if (LI_HANDLER_GO_ON != (res = li_chunk_parser_next(vr, &ctx->chunk_ctx, &p, &pe))) return res;

		%% write exec;

		li_chunk_parser_done(&ctx->chunk_ctx, p - ctx->chunk_ctx.buf);
	}

	if (li_dovecot_auth_parser_has_error(ctx)) return LI_HANDLER_ERROR;
	if (li_dovecot_auth_parser_is_finished(ctx)) {
		/* sanity check: if the whole http request header is larger than 64kbytes, then something probably went wrong */
		if (ctx->chunk_ctx.bytes_in > 64*1024) {
			VR_INFO(vr,
				"request header too large. limit: 64kb, received: %s",
				li_counter_format((guint64)ctx->chunk_ctx.bytes_in, COUNTER_BYTES, vr->wrk->tmp_str)->str
			);

			vr->response.http_status = 413; /* Request Entity Too Large */
			return LI_HANDLER_ERROR;
		}

		li_chunkqueue_skip(ctx->chunk_ctx.cq, ctx->chunk_ctx.bytes_in);
		return LI_HANDLER_GO_ON;
	}
	return LI_HANDLER_ERROR;
}
