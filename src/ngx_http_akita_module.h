/*
 * Copyright (C) 2023 Akita Software
 */

#ifndef _NGX_HTTP_AKITA_MODULE_H_INCLUDED
#define _NGX_HTTP_AKITA_MODULE_H_INCLUDED

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

/* Context for a particular HTTP request */
typedef struct {
  /* Have we already handled this request? */
  ngx_int_t      status;

  /* Time when request is first observed and when its body is available */
  struct timeval request_start;
  struct timeval request_arrived;

  /* TODO: Buffered response data. */
} ngx_http_akita_ctx_t;

#endif /* _NGX_HTTP_AKITA_MODULE_H_INCLUDED */
