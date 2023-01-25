/*
 * Copyright (C) 2022-2023 Akita Software
 */
#include "ngx_http_akita_module.h"
#include <ngx_http_request.h>
#include "akita_client.h"

static ngx_int_t ngx_http_akita_subrequest_callback(ngx_http_request_t *r, void * data, ngx_int_t rc );
static void * ngx_http_akita_create_loc_conf(ngx_conf_t *cf);
static char * ngx_http_akita_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static char * ngx_http_akita_create_upstream(ngx_conf_t *cf, ngx_http_akita_loc_conf_t *akita_conf, ngx_str_t host);
static ngx_int_t ngx_http_akita_precontent_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_akita_response_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_akita_response_body_filter(ngx_http_request_t *r, ngx_chain_t *chain);
static ngx_int_t ngx_http_akita_init(ngx_conf_t *cf);
static ngx_int_t ngx_http_akita_send_request_to_upstream(ngx_http_request_t *subreq, ngx_http_upstream_conf_t *upstream);
static ngx_int_t ngx_http_akita_agent_create_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_akita_agent_reinit_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_akita_agent_process_status_line(ngx_http_request_t *r);
static ngx_int_t ngx_http_akita_agent_process_headers(ngx_http_request_t *r);
static void ngx_http_akita_agent_abort_request(ngx_http_request_t *r);
static void ngx_http_akita_agent_finalize_request(ngx_http_request_t *r, ngx_int_t rc);


static const ngx_uint_t default_max_body = 1 * 1024 * 1024;
static const char default_agent_address[] = "localhost:50800";
static const char *upstream_module_name = "akita";

/* Create the Akita configuration.
 *
 * Returns the configuration on success; NULL otherwise.
 */
static void *
ngx_http_akita_create_loc_conf(ngx_conf_t *cf) {
  ngx_http_akita_loc_conf_t *conf;

  conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_akita_loc_conf_t));
  if (conf == NULL) {
    return NULL;
  }

  conf->max_body_size = NGX_CONF_UNSET_SIZE;
  conf->enabled = NGX_CONF_UNSET;
  ngx_str_set(&conf->upstream.module, upstream_module_name);

  return conf;
}

/* Merge a parent Akita configuration (global or server) into the child configuration (server or location)  */
static char *
ngx_http_akita_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child) {
  ngx_http_akita_loc_conf_t *prev = parent;
  ngx_http_akita_loc_conf_t *conf = child;
  
  ngx_conf_merge_str_value(conf->agent_address, prev->agent_address, default_agent_address);
  ngx_conf_merge_size_value(conf->max_body_size, prev->max_body_size, default_max_body);
  ngx_conf_merge_value(conf->enabled, prev->enabled, 0);

  /* 
   * There are a whole pile of configuration options available for
   * the upstream call to Akita. We're going to default as many of
   * them to the same value that proxy_pass sets up, in 
   * ngx_http_proxy_merge_loc_conf, because we know those settings work. 
   * Some depend on compilation options. 
   */
  conf->upstream.store = 0;
  conf->upstream.buffering = 1;
  conf->upstream.request_buffering = 1;
  conf->upstream.ignore_client_abort = 0;
  conf->upstream.force_ranges = 0;
  conf->upstream.local = NULL;
  conf->upstream.socket_keepalive = 0;
  /* TODO: these seem kind of long, reduce? */
  conf->upstream.connect_timeout = 60000;  /* Connection timeout in ms */
  conf->upstream.send_timeout = 60000;
  conf->upstream.read_timeout = 60000;
  conf->upstream.next_upstream_timeout = 0;
  conf->upstream.send_lowat = 0;
  conf->upstream.buffer_size = (size_t)ngx_pagesize;
  conf->upstream.limit_rate = 0;
  conf->upstream.bufs.num = 8;
  conf->upstream.bufs.size=(size_t)ngx_pagesize;
  conf->upstream.busy_buffers_size_conf = 2 * conf->upstream.buffer_size;
  conf->upstream.busy_buffers_size = 2 * conf->upstream.buffer_size;
  conf->upstream.temp_file_write_size_conf = 2 * conf->upstream.buffer_size;
  conf->upstream.temp_file_write_size = 2 * conf->upstream.buffer_size;
  conf->upstream.max_temp_file_size_conf = 0; /* disabled, no temp files */
  conf->upstream.max_temp_file_size = 0;
  conf->upstream.ignore_headers = NGX_CONF_BITMASK_SET;
  conf->upstream.next_upstream = (NGX_CONF_BITMASK_SET
                                  |NGX_HTTP_UPSTREAM_FT_ERROR
                                  |NGX_HTTP_UPSTREAM_FT_TIMEOUT);
  /* conf->upstream.temp_path left unset */
#if (NGX_HTTP_CACHE)
  conf->upstream.cache = 0;
  /* all other cache settings left unset */  
#endif

  /* These settings will, I think, apply to the subrequest we create */
  /* TODO: I cannot find where either of these are used by upstream; it looks like only proxy 
   * modules read them back out. */
  conf->upstream.pass_request_headers = 1;
  conf->upstream.pass_request_body = 1;
  conf->upstream.intercept_errors = 0;

#if (NGX_HTTP_SSL)
  conf->upstream.ssl = NULL;
  /* all other SSL settings left unset */
#endif

  if (conf->upstream.upstream == NULL) {
    if (prev->upstream.upstream != NULL) {
      /* Copy the pointer to the upstream that was registered earlier! */
      conf->upstream.upstream = prev->upstream.upstream;
    } else if (conf->enabled) {
      /* Create a new upstream using the configured address. */
      return ngx_http_akita_create_upstream(cf, conf, conf->agent_address);
    }
  }

  return NGX_CONF_OK;
}

/* 
 * Create an upstream destination for communicating with the Akita agent.
 * The host name may include a port number; if not the default port 50800
 * will be used.
 */
static char *
ngx_http_akita_create_upstream(ngx_conf_t *cf,
                               ngx_http_akita_loc_conf_t *akita_conf, ngx_str_t host) {
  ngx_url_t u;

  /* Construct a URL to hold the agent address. */
  /* TODO: check for unnecessary http? Or trailing value? */
  ngx_memzero(&u, sizeof(ngx_url_t));
  u.url.len = host.len;
  u.url.data = host.data;
  u.default_port = 50080; /* if no port specified */  
  u.uri_part = 1;
  u.no_resolve = 1; /* defer resolution until needed? */

  /* Create an upstream for the agent.  The rest of the configuration is filled in
     separately, in merge */
  akita_conf->upstream.upstream = ngx_http_upstream_add(cf, &u, 0);
  if (akita_conf->upstream.upstream == NULL) {
    return NGX_CONF_ERROR;
  }
  
  return NGX_CONF_OK;
}

/*
 * Implement the 'akita_agent' configuration directive by creating an
 * upstream to the given hostname.
 */
static char *
ngx_http_akita_agent(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
  ngx_str_t *value;
  ngx_http_akita_loc_conf_t *akita_conf = conf;

  if (akita_conf->upstream.upstream) {
    /* The error message is '"akita_agent" directive <return value>' in <file location>' */
    return "is duplicate";
  }
  
  value = cf->args->elts;
  value = &value[1]; /* I don't know why this is correct. */

  return ngx_http_akita_create_upstream(cf, akita_conf, *value);
}


/* Configuration directives provided by this module. */
static ngx_command_t ngx_http_akita_commands[] = {
  /* Specifies the network address of the akita agent. */
  { ngx_string("akita_agent"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_http_akita_agent,
    NGX_HTTP_LOC_CONF_OFFSET,
    0,
    NULL },
  /* Enable mirroring for a location, server, or globally. */
  { ngx_string("akita_enable"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_flag_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof( ngx_http_akita_loc_conf_t, enabled ),
    NULL },
  /* Set the maximum body size to capture */
  { ngx_string("akita_max_body_size"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_size_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_akita_loc_conf_t, max_body_size),
    NULL },
  ngx_null_command
};


/* The next header-filter handler in the chain. Must be called by our handler
 * once it is done its work.
 */
static ngx_http_output_header_filter_pt ngx_http_next_header_filter;
/* The next body-filter handler in the chain. Must be called by our handler
 * once it is done its work.
 */
static ngx_http_output_body_filter_pt ngx_http_next_body_filter;
 
/* Post-configuration handler for initializing the Akita module. */
static ngx_int_t
ngx_http_akita_init(ngx_conf_t *cf) {
  ngx_http_handler_pt *h;
  ngx_http_core_main_conf_t *cmcf;
  ngx_int_t rc;
  
  /* Initialize the client settings (just variable indexes for now.) */
  rc = ngx_akita_client_init(cf);
  if (rc != NGX_OK) {
    return NGX_ERROR;
  }
  
  /* Register our observer in the precontent phase. */
  cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);  
  h = ngx_array_push(&cmcf->phases[NGX_HTTP_PRECONTENT_PHASE].handlers);
  if (h == NULL) {
    return NGX_ERROR;
  }
  *h = ngx_http_akita_precontent_handler;

  /* Install our header filter */
  ngx_http_next_header_filter = ngx_http_top_header_filter;
  ngx_http_top_header_filter = ngx_http_akita_response_header_filter;
  
  /* Install our body filter */
  ngx_http_next_body_filter = ngx_http_top_body_filter;
  ngx_http_top_body_filter = ngx_http_akita_response_body_filter;
  
  return NGX_OK;  
}

static ngx_http_module_t ngx_http_akita_module_ctx = {
  NULL, /* pre-configuration */
  ngx_http_akita_init, /* post-configuration */
  NULL, /* create main configuration */
  NULL, /* init main configuration */
  NULL, /* create server configuration */
  NULL, /* merge server configuration */
  ngx_http_akita_create_loc_conf, /* create location configuration */
  ngx_http_akita_merge_loc_conf, /* merge location configuration */
};

ngx_module_t ngx_http_akita_module = {
  NGX_MODULE_V1,
  &ngx_http_akita_module_ctx,
  ngx_http_akita_commands,
  NGX_HTTP_MODULE,
  NULL, /* init master */
  NULL, /* init module */
  NULL, /* init process */
  NULL, /* init thread */
  NULL, /* exit thread */
  NULL, /* exit process */
  NULL, /* exit master */
  NGX_MODULE_V1_PADDING,
};


/* API paths for agent */
static ngx_str_t ngx_http_akita_request_location = ngx_string( "/trace/v1/request" );
static ngx_str_t ngx_http_akita_response_location = ngx_string( "/trace/v1/response" );

/* Relays a request to the Akita Agent. To indicate that the we are done
 * processing the request, the status in the request's context is set to
 * DECLINED. Called when the request is fully read.
 */
static void
ngx_http_akita_body_callback(ngx_http_request_t *r) {
  ngx_http_akita_loc_conf_t *akita_config;
  ngx_http_akita_ctx_t *ctx;

  if (r->request_body == NULL ) {
    ngx_log_error( NGX_LOG_INFO, r->connection->log, 0,
                   "Null request body" );    
    return;
  }

  /* Record (approximate) time of last byte of body */
  ctx = ngx_http_get_module_ctx(r, ngx_http_akita_module );
  if (ctx == NULL) {
    return;
  }
  ngx_gettimeofday( &ctx->request_arrived );

  /* Allocate callback structure from pool */
  ngx_http_post_subrequest_t *callback = ngx_pcalloc(r->connection->pool, sizeof( ngx_http_post_subrequest_t ));
  if (callback == NULL) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "Failed to allocate callback" );    
    return;
  }
  callback->handler = ngx_http_akita_subrequest_callback;
  callback->data = NULL;

  /* Retrieve maximum size from configuration */
  akita_config = ngx_http_get_module_loc_conf(r, ngx_http_akita_module);
  if (akita_config == NULL) {
    ngx_log_error( NGX_LOG_INFO, r->connection->log, 0,
                   "No Akita configuration in callback" );
    return;
  }
  
  /* Send the request metadata and body to Akita */
  if (ngx_akita_send_request_body(r, ngx_http_akita_request_location, ctx, akita_config, callback) != NGX_OK) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "Failed to send request body to Akita agent" );
    /* Fall through and continue to send the real request! */
  }

  /* Record that we should respond with DECLINED the next time
     the same request hits our handler. */
  ctx->status = NGX_DECLINED;
  
  /* Re-run the original request chain to send the request
     to its final destination. */
  r->preserve_body = 1;
  r->write_event_handler = ngx_http_core_run_phases;
  ngx_http_core_run_phases(r);
}

/* For each incoming request, check whether mirroring is enabled.
 * Read the request and set up a context to track status. 
 * After the request has been fully read, pass the request on 
 * to the real hander.
 * 
 * Also, when we see a subrequest created by ourselves, send
 * it to the upstream that has been configured for the original location,
 */
static ngx_int_t
ngx_http_akita_precontent_handler(ngx_http_request_t *r) {
  ngx_http_akita_loc_conf_t *akita_config;
  ngx_http_akita_ctx_t *ctx;

  /* Only mirror the main request, not subrequests */
  if (r != r->main) {
    /* Check if this subrequest was initiated by us */
    ctx = ngx_http_get_module_ctx(r, ngx_http_akita_module);
    if (ctx && ctx->subrequest_upstream) {
      return ngx_http_akita_send_request_to_upstream(r, ctx->subrequest_upstream);
    }
    
    return NGX_DECLINED;
  }

  akita_config = ngx_http_get_module_loc_conf(r, ngx_http_akita_module);
  if ( akita_config == NULL || !akita_config->enabled ) {    
    /* Not enabled for this location. */
    return NGX_DECLINED;
  }

  /* If we've already processed this main request, it will have a
     context; return whatever that context tells us to. */
  ctx = ngx_http_get_module_ctx(r, ngx_http_akita_module);
  if (ctx) {
    return ctx->status;
  }

  /* Create a context for this request, set the status to DONE
     initially. After reading the body, we'll switch to DECLINED
     so the real handler can get it. */
  ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_akita_ctx_t));
  if (ctx == NULL) {
    return NGX_ERROR;
  }
  ctx->status = NGX_DONE;  
  ngx_http_set_ctx(r, ctx, ngx_http_akita_module);

  /* Record arrival time at microsecond granularity */
  ngx_gettimeofday( &ctx->request_start );

  /* Set a callback for when entire body is available */
  ngx_int_t rc = ngx_http_read_client_request_body( r, ngx_http_akita_body_callback );
  if ( rc >= NGX_HTTP_SPECIAL_RESPONSE ) {
    return rc;
  }

  /* This is what the mirror module does. When the body finally arrives,
     we will re-run the phases to ensure the request gets to its real destination.
     I think the finalize decreases the reference count on the request.
     TODO: an example module has some code to decrease it manually, see
     https://github.com/klestoff/read-request-body-nginx-module/blob/master/ngx_http_read_request_body_module.c
  */
  ngx_http_finalize_request(r, NGX_DONE);
  return NGX_DONE;                 
} 

/* Logs the status code code from upstream calls to the Akita agent */ 
static ngx_int_t
ngx_http_akita_subrequest_callback(ngx_http_request_t *r, void * data, ngx_int_t rc ) {
  /* TODO: when it's an error, disable and set a timer (with backoff) to re-enable. */

  ngx_uint_t severity = NGX_LOG_DEBUG;
  if (r->headers_out.status != 200 || rc != NGX_OK ) {
    severity = NGX_LOG_WARN;
  }
  ngx_log_error( severity, r->connection->log, 0,
                 "Return code %d from subrequest, HTTP status code %d",
                 rc,
                 r->headers_out.status);
  
  return NGX_OK;
}

/* Called when a response is available.
 * Mirrors response status code and headers to the Akita agent.
 */
static ngx_int_t
ngx_http_akita_response_header_filter(ngx_http_request_t *r) {
  ngx_http_akita_loc_conf_t *akita_config;
  ngx_http_akita_ctx_t *ctx;

  /* Only operate on the main request (in particular, not on our own subrequest!) */
  if ( r != r->main ) {
    return ngx_http_next_header_filter(r);    
  }

  akita_config = ngx_http_get_module_loc_conf(r, ngx_http_akita_module);
  if ( akita_config == NULL || !akita_config->enabled ) {    
    /* Not enabled for this location. */
    return ngx_http_next_header_filter(r);
  }

  /* Record time when upstream (or nginx) sent its response */
  ctx = ngx_http_get_module_ctx(r, ngx_http_akita_module );
  if (ctx == NULL) {
    /* No context == did not go through body callback */
    return ngx_http_next_header_filter(r);
  }
  ngx_gettimeofday( &ctx->response_start );
  ctx->enabled = 1;
  
  if (ngx_akita_start_response_body(r, ctx) != NGX_OK) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "Failed to mirror response to Akita agent." );
    ctx->enabled = 0;
  }
  
  return ngx_http_next_header_filter(r);
}

/* Handles each portion of the HTTP response, adding it to the in-flight
 * body and kicking off the subrequest when done
 */
static ngx_int_t
ngx_http_akita_response_body_filter(ngx_http_request_t *r, ngx_chain_t *chain) {
  ngx_http_akita_loc_conf_t *akita_config;
  ngx_http_akita_ctx_t *ctx;
  ngx_chain_t *curr;
  ngx_http_post_subrequest_t *callback;
  
  if ( r != r->main ) {
    return ngx_http_next_body_filter(r, chain);
  }
  
  akita_config = ngx_http_get_module_loc_conf(r, ngx_http_akita_module);
  if ( akita_config == NULL || !akita_config->enabled ) {    
    /* Not enabled for this location. */
    return ngx_http_next_body_filter(r, chain);
  }

  ctx = ngx_http_get_module_ctx(r, ngx_http_akita_module );
  if ( ctx == NULL || !ctx->enabled) {
    return ngx_http_next_body_filter(r, chain);
  }
  
  for (curr = chain; curr != NULL; curr = curr->next ) {
    if (ngx_akita_append_response_body(r, ctx, akita_config, curr->buf) != NGX_OK) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                    "Failed to append body to Akita API call.");
      /* Don't process the rest of the body (and potentially cause a splice.) */
      ctx->enabled = 0;
      /* Always call the next filter, even if we have an error */
      break;
    }
        
    if (curr->buf->last_buf) {
      ngx_gettimeofday(&ctx->response_complete);

      /* Allocate a callback struct to get the status code */
      callback = ngx_pcalloc(r->connection->pool, sizeof(ngx_http_post_subrequest_t));
      if (callback == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "Failed to allocate callback");
        ctx->enabled = 0;
        break;
      }
      callback->handler = ngx_http_akita_subrequest_callback;
      callback->data = NULL;
      if (ngx_akita_finish_response_body(r, ngx_http_akita_response_location,
                                         ctx,
                                         akita_config,
                                         callback) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "Failed to mirror response to Akita agent");
        ctx->enabled = 0;
      }
      break;      
    }   
  }
  
  return ngx_http_next_body_filter(r, chain);
}


/*
 * Send a subrequest (that's arrived at our content handler) to the specified upstream
 * configuration. Sets up handlers for each of the upstream callbacks.
 */
static ngx_int_t
ngx_http_akita_send_request_to_upstream(ngx_http_request_t *subreq, ngx_http_upstream_conf_t *upstream_conf) {
  ngx_http_upstream_t *u;
  ngx_uint_t content_length;
  
  /* Assign the subrequest to the upstream corresponding to the original request.
   * Note: http_upstream_create overwrites headers_in.content_length_n.
   */
  content_length = subreq->headers_in.content_length_n;
  if (ngx_http_upstream_create(subreq) != NGX_OK) {
    ngx_log_error( NGX_LOG_ERR, subreq->connection->log, 0,
                   "Could not assign upstream" );    
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }
  subreq->headers_in.content_length_n = content_length;

  u = subreq->upstream;
  ngx_str_set(&u->schema, "http://");
  u->conf = upstream_conf;

  u->create_request = ngx_http_akita_agent_create_request;
  u->reinit_request = ngx_http_akita_agent_reinit_request;
  u->process_header = ngx_http_akita_agent_process_status_line;
  u->abort_request = ngx_http_akita_agent_abort_request;
  u->finalize_request = ngx_http_akita_agent_finalize_request;
  /* subreq->state = 0; -- proxy_module does this, why? */

  ngx_http_upstream_init( subreq );
  return NGX_DONE;
}

/* Callbacks from upstream */

/* Create the HTTP header for the request and link its chain
 * into the upstream. */
static ngx_int_t
ngx_http_akita_agent_create_request(ngx_http_request_t *r) {
  ngx_buf_t *b;
  ngx_chain_t *cl;
  size_t header_len;
  
  ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                "create upstream request");

  /* Create HTTP request string and minimal headers */
  /* If we try to use HTTP/1.1, the response handling gets stuck somewhere,
   * eventually timing out. I think this might be because the connection is
   * still open but not sure how to set Nginx up to handle it.
   */
  header_len = sizeof("POST HTTP/1.0" CRLF
                      "Content-Tength: 12345678901234567890" CRLF
                      "Content-Type: application/json" CRLF
                      "Host: api.akitasoftware.com" CRLF CRLF ) - 1 +
    r->uri.len;
  b = ngx_create_temp_buf(r->pool, header_len);
  if (b == NULL) {
    return NGX_ERROR;
  }

  cl = ngx_alloc_chain_link(r->pool);
  if (cl == NULL) {
    return NGX_ERROR;
  }

  b->last = ngx_slprintf(b->pos,b->end, "POST %V HTTP/1.0" CRLF "Content-Length: %d" CRLF "Content-Type: application/json" CRLF "Host: api.akitasoftware.com" CRLF CRLF,
                         &r->uri, r->headers_in.content_length_n );
    
  /* Hook it to the head of the upstream request bufs */
  cl->buf = b;
  cl->next = r->upstream->request_bufs;
  r->upstream->request_bufs = cl;
  
  return NGX_OK;
}

/* Called when an agent request is restarted; can be a no-op */
static ngx_int_t
ngx_http_akita_agent_reinit_request(ngx_http_request_t *r) {
  ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                "reinit upstream request");
  return NGX_OK;
}

/* Called when the agent sends data back to us; must fill in the 
 * upstream's version of the HTTP headers_in structure in order
 * for Nginx to treat it as a HTTP response */
static ngx_int_t
ngx_http_akita_agent_process_status_line(ngx_http_request_t *r) {
  ngx_int_t rc;
  ngx_http_upstream_t *u;
  ngx_http_status_t status;
  ngx_str_t inbound;
  ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                "process upstream response status line");

  u = r->upstream;
  ngx_memzero(&status, sizeof(ngx_http_status_t));
  rc = ngx_http_parse_status_line(r, &u->buffer, &status );
  if (rc == NGX_AGAIN) {
    return rc;
  }

  if (rc == NGX_ERROR) {
    inbound.len = ngx_buf_size( &u->buffer );
    inbound.data = u->buffer.pos;
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "Akita agent did not sent a valid HTTP header: '%V'", &inbound );
    return NGX_OK;
  }

  u->headers_in.status_n = status.code;
  u->process_header = ngx_http_akita_agent_process_headers;
  
  return ngx_http_akita_agent_process_headers(r);
}

/* Called to process the rest of the headers the agent
 * sends back, but we don't actually care, we just need to fix up the
 * upstream state. */
static ngx_int_t
ngx_http_akita_agent_process_headers(ngx_http_request_t *r) {
  ngx_int_t rc;
  ngx_table_elt_t *h;
  ngx_int_t content_length_parsed;
  
  while (1) {
    /* This function manipulates the r->header_* fields and the input chain. */
    rc = ngx_http_parse_header_line(r, &r->upstream->buffer, 1);
            
    if (rc == NGX_OK) {
      /* Handle content-length but ignore other headers. Copy the header data into 
         a fresh buffer instead of just referencing it in-place.  */
      if (ngx_strncasecmp( (unsigned char*)"content-length",
                           r->header_name_start,
                           r->header_name_end - r->header_name_start ) == 0 ) {
        h = ngx_list_push(&r->upstream->headers_in.headers);
        if (h == NULL) {
          return NGX_ERROR;
        }
        h->key.len = r->header_name_end - r->header_name_start;
        h->value.len = r->header_end - r->header_start; /* header_start = start of value, not start of entire thing */        
        h->key.data = ngx_pcalloc(r->pool, h->key.len + h->value.len + 2); /* Include null termination? */
        h->value.data = h->key.data + h->key.len + 1;
        ngx_memcpy(h->key.data, r->header_name_start, h->key.len);
        ngx_memcpy(h->value.data, r->header_start, h->value.len);

        r->upstream->headers_in.content_length = h;
        content_length_parsed = ngx_atoof(h->value.data, h->value.len);
        if (content_length_parsed == NGX_ERROR) {
          ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                        "Invalid Content-Length header from agent");
          return NGX_HTTP_UPSTREAM_INVALID_HEADER;
        }
        r->upstream->headers_in.content_length_n = content_length_parsed;
      }
      
      continue;
    }
    if (rc == NGX_HTTP_PARSE_HEADER_DONE) {
      /* TODO: handle persistent connection to agent, for efficiency. */
      r->upstream->keepalive = 0;
      r->upstream->upgrade = 0;
      return NGX_OK;
    }
    if (rc == NGX_AGAIN) {
      /* Incomplete header in the current buffer */
      return rc;
    }
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "Invalid header from agent: \"%*s\"",
                  r->header_end - r->header_name_start, /* length */
                  r->header_name_start);
    return NGX_HTTP_UPSTREAM_INVALID_HEADER;
  }
}

/* Called when an agent request is aborted by Nginx (in response to a client abort); can be a no-op */
static void
ngx_http_akita_agent_abort_request(ngx_http_request_t *r) {
  ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                "abort upstream request");
}

/* Called when an agent request has been finalized; can be a no-op */
static void
ngx_http_akita_agent_finalize_request(ngx_http_request_t *r, ngx_int_t rc) {
  ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                "finalize_upstream_request");
}

