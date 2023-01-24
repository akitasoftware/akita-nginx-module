/*
 * Copyright (C) 2022-2023 Akita Software
 */
#include "ngx_http_akita_module.h"
#include <ngx_http_request.h>
#include "akita_client.h"

static ngx_int_t
ngx_http_akita_subrequest_callback(ngx_http_request_t *r, void * data, ngx_int_t rc );
static void *
ngx_http_akita_create_loc_conf(ngx_conf_t *cf);
static char *
ngx_http_akita_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t
ngx_http_akita_precontent_handler(ngx_http_request_t *r);
static ngx_int_t
ngx_http_akita_response_header_filter(ngx_http_request_t *r);
static ngx_int_t
ngx_http_akita_response_body_filter(ngx_http_request_t *r, ngx_chain_t *chain);
static ngx_int_t
ngx_http_akita_init(ngx_conf_t *cf);

static const ngx_uint_t default_max_body = 1 * 1024 * 1024;
static const char default_agent_address[] = "localhost:50800";

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
  return NGX_CONF_OK;
}

/* Configuration directives provided by this module. */
static ngx_command_t ngx_http_akita_commands[] = {
  /* Specifies the network address of the akita agent. */
  { ngx_string("akita_agent"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof( ngx_http_akita_loc_conf_t, agent_address ),
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


/* Temporary location of proxy that sends to the agent. TBR with an upstream */
/* TODO: move to configuration before that gets done? */
static ngx_str_t ngx_http_akita_request_location = ngx_string( "/akita/trace/v1/request" );
static ngx_str_t ngx_http_akita_response_location = ngx_string( "/akita/trace/v1/response" );

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
 */
static ngx_int_t
ngx_http_akita_precontent_handler(ngx_http_request_t *r) {
  ngx_http_akita_loc_conf_t *akita_config;
  ngx_http_akita_ctx_t *ctx;

  /* Only execute on the main request, not subrequests */
  if (r != r->main) {
    return NGX_DECLINED;
  }

  akita_config = ngx_http_get_module_loc_conf(r, ngx_http_akita_module);
  if ( akita_config == NULL || !akita_config->enabled ) {    
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

static ngx_int_t
ngx_http_akita_subrequest_callback(ngx_http_request_t *r, void * data, ngx_int_t rc ) {
  ngx_log_error( NGX_LOG_INFO, r->connection->log, 0,
                 "Return code %d from subrequest, HTTP response %d",
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
  ngx_gettimeofday( &ctx->response_start );
  ctx->enabled = 1;
  
  if (ngx_akita_start_response_body(r, ctx) != NGX_OK) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "Failed to mirror response to Akita agent." );
    ctx->enabled = 0;
  }
  
  return ngx_http_next_header_filter(r);
}

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
