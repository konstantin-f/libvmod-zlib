/*
 * Copyright 2017 Thomson Reuters
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

/* need vcl.h before vrt.h for vmod_evet_f typedef */
#include "vdef.h"
#include "vcl.h"
#include "vrt.h"
#include "cache/cache.h"
#include "lib/libvgz/vgz.h"
#include "vcc_zlib_if.h"
#include "vsb.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define READ_BUFFER_SIZE 8192

#define VMOD_ZLIB_DEBUG
#ifdef VMOD_ZLIB_DEBUG
#define DEBUG(x) x
#else
#define DEBUG(x) (void);
#endif

static const struct gethdr_s VGC_HDR_REQ_Content_2d_Length =
    { HDR_REQ, "\017Content-Length:"};

static void
clean(void *priv)
{
	struct vsb	*vsb;

	if (priv) {
		CAST_OBJ_NOTNULL(vsb, *(struct vsb**)priv, VSB_MAGIC);
		VSB_destroy(&vsb);
	}
}

static struct vsb**
VSB_get(VRT_CTX, struct vmod_priv *priv)
{
	struct vsb **pvsb;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(priv);
	if(priv->priv == NULL) {
		priv->priv = malloc(sizeof(pvsb));
		pvsb = (struct vsb **)priv->priv;
		*pvsb = VSB_new_auto();
		priv->free = clean;
	}
	return (pvsb);
}

static int
fill_pipeline(VRT_CTX, struct vsb** pvsb, struct http_conn *htc, ssize_t len)
{
	struct vsb	*vsb;
	char		*buffer;
	ssize_t		l;
	ssize_t		i;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CAST_OBJ_NOTNULL(vsb, *pvsb, VSB_MAGIC);
	CHECK_OBJ_NOTNULL(htc, HTTP_CONN_MAGIC);
	AN(len > 0);
	l = 0;
	if (htc->pipeline_b) {
		l = htc->pipeline_e - htc->pipeline_b;
		AN(l > 0);
		if (l >= len) {
			return (l);
		}
		AZ(VSB_bcat(vsb, htc->pipeline_b, l));
		len -= l;
	}

	i = 0;
	buffer = WS_Alloc(ctx->ws, READ_BUFFER_SIZE);
	while (len > 0) {
		i = read(htc->fd, buffer, READ_BUFFER_SIZE);
		if (i < 0) {
			if (htc->pipeline_b == htc->pipeline_e) {
				htc->pipeline_b = NULL;
				htc->pipeline_e = NULL;
			}
			// XXX: VTCP_Assert(i); // but also: EAGAIN
			VSLb(ctx->req->htc->vfc->wrk->vsl, SLT_FetchError,
			    "%s", strerror(errno));
			ctx->req->req_body_status = REQ_BODY_FAIL;
			return (i);
		}
		AZ(VSB_bcat(vsb, buffer, i));
		len -= i;
	}
	VSB_finish(vsb);
	AN(VSB_len(vsb) > 0);
	htc->pipeline_b = VSB_data(vsb);
	htc->pipeline_e = htc->pipeline_b + VSB_len(vsb);
	return (VSB_len(vsb));
}

/* -------------------------------------------------------------------------------------/
   Decompress one part
   Note that we are using libzlib (from varnish) and not zlib (from system)
*/
ssize_t uncompress_pipeline(VRT_CTX, struct vsb** pvsb, struct http_conn *htc)
{
	struct vsb	*body;
	struct vsb	*output;
	z_stream	*stream;
	char		*buffer;
	int		err;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	stream = WS_Alloc(ctx->ws, sizeof(*stream));
	XXXAN(stream);
	if (inflateInit2(stream, 31) != Z_OK) {
		VSLb(ctx->vsl, SLT_Error, "zlib: can't run inflateInit2");
		return (-1);
	}

	output = VSB_new_auto();
	buffer = WS_Alloc(ctx->ws, cache_param->gzip_buffer);

	stream->next_in = (Bytef *)htc->pipeline_b;
	stream->avail_in = htc->pipeline_e - htc->pipeline_b;
	while (stream->avail_in > 0) {
		stream->next_out = (Bytef *)buffer;
		stream->avail_out = cache_param->gzip_buffer;

		DEBUG(VSLb(ctx->vsl, SLT_Debug, "zlib: inflateb in (%lu/%u) out (%lu/%u)",
			(uintptr_t)stream->next_in, stream->avail_in,
			(uintptr_t)stream->next_out, stream->avail_out));
		err = inflate(stream, Z_SYNC_FLUSH);
		DEBUG(VSLb(ctx->vsl, SLT_Debug, "zlib: inflatef in (%lu/%u) out (%lu/%u)",
			(uintptr_t)stream->next_in, stream->avail_in,
			(uintptr_t)stream->next_out, stream->avail_out));
		if (err != Z_OK && err != Z_STREAM_END) {
			VSLb(ctx->vsl, SLT_Error, "zlib: inflate read buffer (%d/%s)", err, stream->msg);
			inflateEnd(stream);
			VSB_destroy(&output);
			return (-1);
		}
		AZ(VSB_bcat(output, buffer, (char*)stream->next_out - buffer));
	}
	inflateEnd(stream);
	VSB_finish(output);

	// We got a complete uncompressed buffer
	// We need to write it back into pipeline
	if (*pvsb) {
		CAST_OBJ_NOTNULL(body, *pvsb, VSB_MAGIC);
		//AN(htc->pipeline_b == VSB_data(body));
		VSB_destroy(pvsb);
	}
	*pvsb = output;
	htc->pipeline_b = VSB_data(output);
	htc->pipeline_e = htc->pipeline_b + VSB_len(output);
	return (VSB_len(output));
}

ssize_t validate_request(VRT_CTX)
{
	const char *ptr;
	ssize_t cl;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	if (ctx->method != VCL_MET_RECV) {
		/* Can be called only in vcl_recv.
		** vcl_backend_fetch is a bad place since std.cache is in recv
		*/
		VSLb(ctx->vsl, SLT_Error, "zlib: must be called in vcl_recv");
		return (-1);
	}

	// Content-Encoding handling
	if (http_GetHdr(ctx->http_req, H_Content_Encoding, &ptr)) {
		if (strstr(ptr, "deflate") != 0) {
			VSLb(ctx->vsl, SLT_Error, "zlib: unsupported Content-Encoding");
			return (-1);
		}
		else if (strstr(ptr, "gzip") == 0) {
			VSLb(ctx->vsl, SLT_Error, "zlib: unsupported Content-Encoding");
			return (-1);
		}
	}
	else {
		VSLb(ctx->vsl, SLT_Debug, "zlib: nothing to do");
		return (0);
	}

	// Transfer-Encoding handling
	if (http_GetHdr(ctx->http_req, H_Transfer_Encoding, &ptr)) {
		VSLb(ctx->vsl, SLT_Error, "zlib: unsupported Transfer-Encoding");
		return (-1);
	}
	// Get Content-Length
	cl = http_GetContentLength(ctx->http_req);
	if (cl <= 0) {
		VSLb(ctx->vsl, SLT_Debug, "zlib: no Content-Length");
		return (cl);
	}
	return (cl);
}

/*--------------------------------------------------------------------
 * Unzip of a request / response
 * Does not manage chunks.
 * Returns :
 * 0 if success or no Content-Length
 * -1 if failed:
 * - unknown Content-Encoding
 * - Transfer-Encoding present
 * - uncompress error
 */
VCL_INT __match_proto__(td_zlib_unzip_request)
	vmod_unzip_request(VRT_CTX, struct vmod_priv *priv)
{
	void		*virgin;
	struct vsb	**pvsb;
	ssize_t		cl;
	ssize_t		ret;

	virgin = WS_Snapshot(ctx->ws);

	cl = validate_request(ctx);
	if (cl <= 0) {
		// can be 0 (no body) or -1 (wrong req)
		return (cl);
	}

	pvsb = VSB_get(ctx, priv);
	ret = fill_pipeline(ctx, pvsb, ctx->req->htc, cl);
	if (ret <= 0) {
		VSLb(ctx->vsl, SLT_Error, "zlib: read error (%ld)", ret);
		WS_Reset(ctx->ws, virgin);
		return (-1);
	}
	AN(ret == cl);
	cl = uncompress_pipeline(ctx, pvsb, ctx->req->htc);
	if (cl < 0) {
		VSLb(ctx->vsl, SLT_Error, "zlib: can't uncompress pipeline");
		WS_Reset(ctx->ws, virgin);
		return (-1);
	}
	else if (cl == 0) {
		http_Unset(ctx->req->http, H_Content_Length);
	}
	else {
		VRT_SetHdr(ctx, &VGC_HDR_REQ_Content_2d_Length, VRT_INT_string(ctx, cl),
		    vrt_magic_string_end);
	}
	ctx->req->htc->content_length = cl;
	http_Unset(ctx->req->http, H_Content_Encoding);

	WS_Reset(ctx->ws, virgin);
	VSLb(ctx->vsl, SLT_Debug, "zlib: completed with success");
	return (0);
}