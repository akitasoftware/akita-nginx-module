/*
 * Copyright (C) 2023 Akita Software
 */

#ifndef _AKITA_NGX_MODULE_AKITA_CLIENT_H_INCLUDED
#define _AKITA_NGX_MODULE_AKITA_CLIENT_H_INCLUDED

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

/* Top-level functions for sending data to the Akita agent. */

/* Initialize the Akita client based on the current configuration. */
ngx_int_t
ngx_akita_client_init(ngx_conf_t *cf);
                      
/*
 * Send a REST call (as a subrequest) reporting on an HTTP request body.
 * Takes the original request and (for now) a path that will proxy to the
 * client. This should be called in a body callback where the entire request
 * body is already available.
 *
 * TODO: replace agent_path with an Nginx upstream.
 */
ngx_int_t
ngx_akita_send_request_body(ngx_http_request_t *r, ngx_str_t agent_path,
                            ngx_http_akita_ctx_t *ctx,
                            ngx_http_akita_loc_conf_t *config,
                            ngx_http_post_subrequest_t *callback);

/*
 * Records the response's metadata and start building its response body.
 * Allocates ctx->response_json from the pool in r. On return,
 * ctx->response_json will have a JSON string for the response body started,
 * but not yet terminated.
 */
ngx_int_t
ngx_akita_start_response_body(ngx_http_request_t *r,
                              ngx_http_akita_ctx_t *ctx);

/*
 * Add a buffer from the response body to the mirrored API call's JSON
 * response. The buffer is JSON-escaped as it is written, and
 * ctx->json_response is assumed to be in the middle of an as-yet unterminated
 * JSON string.
 */
ngx_int_t
ngx_akita_append_response_body(ngx_http_request_t *r,
                               ngx_http_akita_ctx_t *ctx,
                               ngx_http_akita_loc_conf_t *config,
                               ngx_buf_t *);

/*
 * Finish sending the response body to Akita, using the partially
 * assembled JSON body in ctx->json_response. Create a new subrequest
 * of the original request r, and send it to agent_path. 
 */
ngx_int_t
ngx_akita_finish_response_body(ngx_http_request_t *r,
                               ngx_str_t agent_path,
                               ngx_http_akita_ctx_t *ctx,
                               ngx_http_akita_loc_conf_t *config,
                               ngx_http_post_subrequest_t *callback);

#endif /* NGX_AKITA_MODULE_AKITA_CLIENT_H_INCLUDED */
