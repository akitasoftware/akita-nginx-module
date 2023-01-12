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
                            ngx_http_post_subrequest_t *callback);

#endif /* NGX_AKITA_MODULE_AKITA_CLIENT_H_INCLUDED */
