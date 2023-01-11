/*
 * Copyright (C) 2022-2023 Akita Software
 */
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_http_request.h>
#include "akita_client.h"

static ngx_int_t
ngx_http_akita_subrequest_callback(ngx_http_request_t *r, void * data, ngx_int_t rc );

/* Location-specific configuration for the Akita module. */
typedef struct {
  /* The network address for the Akita agent REST API.*/  
  ngx_str_t agent_address;
  
} ngx_http_akita_loc_conf_t;

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

  ngx_str_null( &(conf->agent_address) );
  return conf;
}

/* Merge a parent Akita configuration into the child configuration.
 * Uses "" as the default value that indicates mirroring is not enabled. */
static char *
ngx_http_akita_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child) {
  ngx_http_akita_loc_conf_t *prev = parent;
  ngx_http_akita_loc_conf_t *conf = child;
  
  ngx_conf_merge_str_value( conf->agent_address, prev->agent_address, "" );
  
  return NGX_CONF_OK;
}

/* Configuration directives provided by this module. */
static ngx_command_t ngx_http_akita_commands[] = {
  /* Enables mirroring of the given location, and specifies the network address of the akita agent. */
  { ngx_string("akita_agent"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof( ngx_http_akita_loc_conf_t, agent_address ),
    NULL },
  ngx_null_command
};

/* Context for a particular HTTP request */
typedef struct {
  /* Have we already handled this request? */
  ngx_int_t     status;

  /* TODO: Server arrival time.
     TODO: Buffered response data. */
} ngx_http_akita_ctx_t;

static ngx_int_t
ngx_http_akita_precontent_handler(ngx_http_request_t *r);

static ngx_int_t
ngx_http_akita_response_header_filter(ngx_http_request_t *r);

static ngx_int_t
ngx_http_akita_response_body_filter(ngx_http_request_t *r, ngx_chain_t *chain);

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


/* Temporary location of proxy that sends to the agent. TBR with an upstream */
/* TODO: move to configuration before that gets done? */
static ngx_str_t ngx_http_akita_location = ngx_string( "/akita" );

/* Relays a request to the Akita Agent. To indicate that the we are done
 * processing the request, the status in the request's context is set to
 * DECLINED. Called when the request is fully read.
 */
static void
ngx_http_akita_body_callback(ngx_http_request_t *r) {
  if (r->request_body == NULL ) {
    ngx_log_error( NGX_LOG_INFO, r->connection->log, 0,
                   "Null request body" );    
    return;
  }

  /* Allocate callback structure from pool */
  ngx_http_post_subrequest_t *callback = ngx_pcalloc(r->connection->pool, sizeof( ngx_http_post_subrequest_t ));
  if (callback == NULL) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "Failed to allocate callback" );    
    return;
  }
  callback->handler = ngx_http_akita_subrequest_callback;
  callback->data = NULL;

  /* Send the request metadata and body to Akita */
  if (ngx_akita_send_request_body(r, ngx_http_akita_location, callback) != NGX_OK) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "Failed to send request body to Akita agent" );
    /* Fall through and continue to send the real request! */
  }

  /* Record that we should respond with DECLINED the next time
     the same request hits our handler. */
  ngx_http_akita_ctx_t  *ctx = ngx_http_get_module_ctx(r, ngx_http_akita_module );
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
 */
static ngx_int_t
ngx_http_akita_precontent_handler(ngx_http_request_t *r) {
  ngx_http_akita_loc_conf_t *akita_config;
  ngx_table_elt_t *host_header = NULL;
  ngx_str_t *host_name = NULL;
  ngx_str_t unknown = ngx_string( "unknown" );
  ngx_list_part_t *header_part;
  ngx_table_elt_t *headers;
  ngx_uint_t i = 0;
  ngx_http_akita_ctx_t *ctx;

  /* Only execute on the main request, not subrequests */
  if (r != r->main) {
    return NGX_DECLINED;
  }

  akita_config = ngx_http_get_module_loc_conf(r, ngx_http_akita_module);
  if ( akita_config == NULL || akita_config->agent_address.len == 0 ) {
    /* Not enabled for this location. */
    return NGX_DECLINED;
  }

  /* Check that context does not already exist. */
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
    
  host_header = r->headers_in.host;
  if ( host_header != NULL ) {
    host_name = &host_header->value;
  } else {
    host_name = &unknown;
  }
  /* TODO: lower to DEBUG level */
  ngx_log_error( NGX_LOG_INFO, r->connection->log, 0,
                 "I saw a request for %V %V %V",
                 &(r->method_name),
                 host_name,
                 &(r->uri));
  
  for (header_part = &(r->headers_in.headers.part); header_part; header_part = header_part->next) {
    headers = header_part->elts;
    for (i = 0; i < header_part->nelts; i++) {
      ngx_log_error( NGX_LOG_INFO, r->connection->log, 0,
                     "Header '%V' value '%V'",
                     &headers[i].key, &headers[i].value );      
    }
  }

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

static ngx_int_t
ngx_http_akita_subrequest_callback(ngx_http_request_t *r, void * data, ngx_int_t rc ) {
  ngx_log_error( NGX_LOG_INFO, r->connection->log, 0,
                 "Response code %d from subrequest", rc );
  return NGX_OK;
}

static unsigned char *intro = (unsigned char *)"{ \"headers\" : [";
static unsigned char *outro = (unsigned char *)"]}";

/* Called when a response is available.
 * Mirrors response status code and headers to the Akita agent.
 */
static ngx_int_t
ngx_http_akita_response_header_filter(ngx_http_request_t *r) {
  ngx_list_part_t *header_part;
  ngx_table_elt_t *headers;
  ngx_http_request_t *sr;
  ngx_uint_t i = 0;
  ngx_int_t rc;

  /* Only operate on the main request (in particular, not on our own subrequest!) */
  if ( r != r->main ) {
    ngx_log_error( NGX_LOG_INFO, r->connection->log, 0,
                  "Skipping non-main response." );
    return ngx_http_next_header_filter(r);    
  }
    
  header_part = &(r->headers_out.headers.part);
  headers = header_part->elts; 
  ngx_log_error( NGX_LOG_INFO, r->connection->log, 0,
                 "Response available with %d headers", header_part->nelts );

  ngx_buf_t *b;
  ngx_chain_t *out;
  ngx_chain_t *link;
  ngx_uint_t content_length = 0;
  ngx_str_t content_length_str;
    
  b = ngx_calloc_buf( r->pool );
  if ( b == NULL ) {
    return NGX_ERROR;
  }
  b->pos = intro;
  b->last = intro + ngx_strlen( intro );
  b->memory = 1;
  b->last_buf = 0;
  content_length += ngx_strlen( intro );
  
  out = ngx_alloc_chain_link( r->pool );
  out->buf = b;
  out->next = NULL;

  /* TODO: copy headers into subrequest chain as JSON */
  /* Nginx-written headers are not present, nor are the ones from 
   * the upstream response that will be overwritten?  See
   * https://forum.nginx.org/read.php?2,225317,225329#msg-225329
   */
  for (header_part = &(r->headers_out.headers.part); header_part; header_part = header_part->next) {
    headers = header_part->elts;
    for (i = 0; i < header_part->nelts; i++) {
      ngx_log_error( NGX_LOG_INFO, r->connection->log, 0,
                     "Response header '%V' value '%V'",
                     &headers[i].key, &headers[i].value );      
    }
  }

  b = ngx_calloc_buf( r->pool );
  if ( b == NULL ) {
    return NGX_ERROR;
  }
  b->pos = outro;
  b->last = outro + ngx_strlen( outro );
  b->memory = 1;
  b->last_buf = 1;
  content_length += ngx_strlen( outro );
  
  link = ngx_alloc_chain_link( r->pool );
  link->buf = b;
  link->next = NULL;
  out->next = link;
  
  /* Allocate callback structure from pool */
  ngx_http_post_subrequest_t *callback = ngx_pcalloc( r->connection->pool, sizeof( ngx_http_post_subrequest_t ) );
  callback->handler = ngx_http_akita_subrequest_callback;
  callback->data = NULL;

  /* Send the callback to a new path for now,
     later replace this by a proper upstream  */ 
  ngx_str_t replacement_uri = ngx_string( "/akita" );
  ngx_str_t query_params = ngx_null_string;

  /* Update the agent with the information we got in the response */
  rc = ngx_http_subrequest( r,
                            &replacement_uri,
                            &query_params,
                            &sr,
                            callback,
                            NGX_HTTP_SUBREQUEST_IN_MEMORY );
  if ( rc >= NGX_HTTP_SPECIAL_RESPONSE ) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "Subrequest return code %d", rc );
  }

  sr->request_body = ngx_pcalloc( r-> pool, sizeof(ngx_http_request_body_t) );
  if ( sr->request_body == NULL ) {
    return NGX_ERROR;
  }
  sr->request_body->bufs = out;

  /* Rewrite the content-length header to match our new body. */
  content_length_str.data = ngx_pcalloc( r->connection->pool, 20 );
  content_length_str.len = ngx_snprintf( content_length_str.data, 20, "%d", content_length ) - content_length_str.data;
  
  /* TODO: check if absent */
  sr->headers_in.content_length->hash = 1;
  sr->headers_in.content_length->value.data = content_length_str.data;    
  sr->headers_in.content_length->value.len = content_length_str.len;    
  sr->headers_in.content_length_n = content_length;
  
  return ngx_http_next_header_filter(r);
}

static ngx_int_t
ngx_http_akita_response_body_filter(ngx_http_request_t *r, ngx_chain_t *chain) {
  /* TODO: mirror response in a subrequest. */
  return ngx_http_next_body_filter(r, chain);
}
