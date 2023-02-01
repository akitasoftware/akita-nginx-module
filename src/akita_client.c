/*
 * Copyright (C) 2023 Akita Software
 */

#include "ngx_http_akita_module.h"
#include "akita_client.h"

/* Functions for generating JSON objects. */

/* A chain of buffers holding the JSON output */
typedef struct json_data_s {
  ngx_pool_t *pool;            /* Pool for new allocations */
  ngx_chain_t *chain;          /* Head of output data */
  ngx_chain_t *tail;           /* Tail of output data */
  ngx_uint_t content_length;   /* Total size of data so far */
  ngx_uint_t oom;              /* Nonzero if OOM hit */
} json_data_t;

static json_data_t* json_alloc( ngx_pool_t *pool );                             
static unsigned char * json_ensure_space( json_data_t *buf, ngx_uint_t size );
static void json_write_char( json_data_t *buf, unsigned char c );
static void json_write_string_literal( json_data_t *buf, ngx_str_t *str );
static void json_write_time_literal( json_data_t *buf, struct timeval *tm  );
static void json_write_uint_property( json_data_t *buf, ngx_str_t *key, ngx_uint_t n );
static void json_snprintf(json_data_t *j, size_t max_len, const char *fmt, ...);
static ngx_int_t json_escape_buf(json_data_t *j, ngx_http_request_t *r, size_t max_size, size_t *total_size, ngx_buf_t *buf);

/* A key and string value to write into a JSON object */
typedef struct json_kv_string_s {
  ngx_str_t key;
  ngx_str_t value;
  ngx_int_t omit;  /* Skip this key in the array */
} json_kv_string_t;

static void json_write_kv_strings( json_data_t *buf, json_kv_string_t *kvs );

static ngx_int_t ngx_akita_get_request_id(ngx_http_request_t *r, ngx_str_t *dest);
static void ngx_akita_write_headers_list(json_data_t *j, ngx_list_t *headers_list);
static void ngx_akita_write_body(json_data_t *j, ngx_http_request_t *r, size_t max_size );
static void ngx_akita_clear_headers(ngx_http_request_t *r);
static ngx_int_t ngx_akita_set_request_size(ngx_http_request_t *r, ngx_uint_t content_length);
static ngx_int_t ngx_akita_set_json_content_type(ngx_http_request_t *r);
static ngx_int_t ngx_akita_send_api_call(ngx_http_request_t *r,
                                         ngx_str_t agent_path,
                                         ngx_http_post_subrequest_t *callback,
                                         ngx_http_akita_loc_conf_t *config,
                                         ngx_chain_t *body,
                                         size_t content_length);
  
static const ngx_uint_t json_initial_size = 4096;

/* Allocate a new buffer for JSON. Returns NULL if the allocation fails. */
static json_data_t *
json_alloc( ngx_pool_t *pool ) {
  ngx_bufs_t bufs;
  json_data_t *j = ngx_pcalloc(pool, sizeof(json_data_t));
  if (j == NULL) {
    return NULL;
  }
  
  j->pool = pool;
  bufs.num = 1;
  bufs.size = json_initial_size;
  j->chain = ngx_create_chain_of_bufs(pool, &bufs );
  if (j->chain == NULL) {
    return NULL;
  }
  j->tail = j->chain;
  j->content_length = 0;
  return j;
}

/*
 * Ensure there is enough space to write "size" bytes, and return
 * a pointer to the start of that space, which will always be in j->tail.
 * The caller is responsible for updating content_length and the
 * buffer's last pointer.
 *
 * If an error occurs, sets `j->oom` and returns NULL.
 */
static unsigned char *
json_ensure_space(json_data_t *j, ngx_uint_t size) {
  ngx_chain_t *cl;
  ngx_buf_t *curr_buf;
  
  /* end-start is the capacity of the buffer, so end is not usable. */
  curr_buf = j->tail->buf;
  if (curr_buf->last + size < curr_buf->end ) {
    return curr_buf->last;
  }

  /* Create a buffer at least large enough, and at least our initial size. */
  if (size > json_initial_size) {
    curr_buf = ngx_create_temp_buf(j->pool, size);
  } else {
    curr_buf = ngx_create_temp_buf(j->pool, json_initial_size);
  }
  if (curr_buf == NULL) {
    j->oom = 1;
    return NULL;
  }

  cl = ngx_alloc_chain_link(j->pool);
  if (cl == NULL) {
    j->oom = 1;
    return NULL;
  }
  cl->buf = curr_buf;
  cl->next = NULL;
  j->tail->next = cl;
  j->tail = cl;
  return curr_buf->last;  
}

/*
 * Write a single character to the JSON buffer. Sets `j->oom` if an error
 * occurs.
 */
static void json_write_char( json_data_t *j, unsigned char c ) {
  unsigned char *p = json_ensure_space( j, 1 );
  if (p == NULL) {
    return;
  }
  *p = c;
  j->content_length++;
  j->tail->buf->last++;
}

/*
 * Write a properly-escaped string literal to the JSON buffer, including "".
 * Sets `j->oom` if an error occurs.
 */
static void json_write_string_literal(json_data_t *j, ngx_str_t *str) {
  unsigned char *dst;
  uintptr_t sz;

  /* The doc says return value is the complete size but it's just the delta */
  sz = ngx_escape_json( NULL, str->data, str->len ) + str->len + 2;
  dst = json_ensure_space( j, sz );
  if (dst == NULL) {
    return;
  }
  *dst++ = '"';
  dst = (unsigned char *)ngx_escape_json(dst, str->data, str->len);
  *dst++ = '"';
  j->content_length += sz;
  j->tail->buf->last = dst;
}

/* Printf to a JSON buffer; may set `j->oom' on failure. */
static void json_snprintf(json_data_t *j, size_t max_len, const char *fmt, ...) {
  u_char *dst, *end;
  va_list args;
  va_start(args, fmt);

  dst = json_ensure_space(j, max_len);
  if (dst == NULL) {
    return;
  }
  
  end = ngx_vslprintf(dst, dst+max_len, fmt, args);
  j->content_length += (end - dst);
  j->tail->buf->last = end;  
}

/* Write a key and an unsigned integer to the JSON buffer.
   Sets 'j->oom' if an error occurs. */
static void json_write_uint_property(json_data_t *j, ngx_str_t *key, ngx_uint_t n) {  
  const int max_decimal_len = 20; /* handles 64-bit unsigned */
  json_write_string_literal(j, key);
  json_write_char(j, ':');
  json_snprintf(j, max_decimal_len, "%ud", n);
}

/*
 * Output an array of key/value pairs. The last pair should be signalled with
 * an null key. Commas appear between pairs but not after the last.
 */
static void json_write_kv_strings(json_data_t *j, json_kv_string_t *kv) {
  ngx_uint_t need_comma = 0;
  
  for (; kv->key.len > 0; kv++) {
    if (kv->omit) {
      continue;
    }

     if (need_comma) {
      json_write_char(j, ',');      
    }

    json_write_string_literal(j, &(kv->key));
    json_write_char(j, ':');
    json_write_string_literal(j, &(kv->value));
    need_comma = 1;
  }
}

/*
 * Output a timestamp as a string literal.  We use RFC3339 format with 
 * microsecond precision and UTC time zone. Note that the ngx_request_t 
 * timestamp is in seconds and milliseconds.
 *
 * Sets `j->oom` if an error occurs.
 */
static void json_write_time_literal(json_data_t *j, struct timeval *tv) {
  static ngx_str_t format = ngx_string("\"2006-01-02T15:04:05.999999Z\"");
  ngx_tm_t tm;
  ngx_gmtime(tv->tv_sec, &tm);
  json_snprintf(j, format.len, "\"%4d-%02d-%02dT%02d:%02d:%02d.%06dZ\"",
                tm.ngx_tm_year, tm.ngx_tm_mon,
                tm.ngx_tm_mday, tm.ngx_tm_hour,
                tm.ngx_tm_min, tm.ngx_tm_sec,
                tv->tv_usec);
}

/* API request schema and subrequest manipulation */

/* Request format:
{
   "request_id": "NNNNN",
   "method": "GET",
   "host": "example.com",
   "path": "/some/path",
   "headers": [
     { "header": "Authorization", "value": "..." },
     ...
   },
   "body" : "....",   // escaped but not Base64encoded?
   "truncated" : 1024000,
   "request_start": "2022-12-07T12:34:56.123456",
   "request_arrived": "2022-12-07T12:34:56.234567",
}
*/

/* Cached index of $request_id variable, determined at configuration time */
static ngx_int_t ngx_request_id_index = 0;

/* 
 * Get the request ID as a string. Returns an Nginx error code.
 * TODO: for ngx prior to 1.11.0, we need to use the connection and
 * connection requests to generate an ID.
 */
static ngx_int_t
ngx_akita_get_request_id(ngx_http_request_t *r, ngx_str_t *dest) {
  ngx_http_variable_value_t *v;
  
  v = ngx_http_get_indexed_variable(r, ngx_request_id_index);
  if (v == NULL || v->not_found) {
    return NGX_ERROR;
  }

  dest->len = v->len;
  dest->data = v->data;
  return NGX_OK;
}

/* Remove all input headers from a (sub-)request. */
static void
ngx_akita_clear_headers(ngx_http_request_t *r) {
  /* Clear all pointers and cached values */
  ngx_memzero(&r->headers_in, sizeof(ngx_http_headers_in_t));

  /* Set up a new list of ngx_table_elt_t. */
  ngx_list_init(&r->headers_in.headers, r->pool, 4, sizeof(ngx_table_elt_t));
}

/* Write the list of headers to the JSON API call */
static void
ngx_akita_write_headers_list(json_data_t *j, ngx_list_t *headers_list ) {
  ngx_list_part_t *header_part;
  ngx_table_elt_t *headers;
  ngx_uint_t i = 0;
  ngx_uint_t need_comma = 0;
  
  static ngx_str_t headers_key = ngx_string( "headers" );
  json_write_string_literal(j, &headers_key);
  json_write_char(j, ':' );
  json_write_char(j, '[' );  
  for (header_part = &(headers_list->part); header_part; header_part = header_part->next) {
    headers = header_part->elts;
    for (i = 0; i < header_part->nelts; i++) {
      if (need_comma) {
        json_write_char(j, ',');
      }
      
      json_kv_string_t header_fields[] = {
        { ngx_string( "header" ), headers[i].key, 0 },
        { ngx_string( "value" ), headers[i].value, 0 },
        { ngx_null_string, ngx_null_string, 0 }        
      };
      json_write_char(j, '{');
      json_write_kv_strings(j, header_fields);
      json_write_char(j, '}');
      need_comma = 1;
    }
  }
  json_write_char(j, ']' );  
}

/*
 * Write the buffer to a JSON string literal.
 * Assumes the quotes have already been added.
  * Update total_size with the actual size.
  * The portion of the buffer that is written to the JSON literal is limited to
  * max_size - *total_size, as counted before characters are escaped.
  *
  * Returns an Nginx error code.
 */
static ngx_int_t
json_escape_buf(json_data_t *j, ngx_http_request_t *r, size_t max_size, size_t *total_size, ngx_buf_t *buf) {
  unsigned char *unescaped, *dst;
  size_t unescaped_len = 0;
  unsigned char *file_buf = NULL;
  ssize_t num_read;
  uintptr_t size_delta;

  /* If at max size, record length */
  if (*total_size >= max_size) {
    *total_size += ngx_buf_size(buf);
    return NGX_OK;
  }

  unescaped_len = ngx_buf_size(buf);
  /* Truncate if over max size */
  if (*total_size + unescaped_len > max_size) {
    unescaped_len = max_size - *total_size;
  }
  /* Record the real size */
  *total_size += ngx_buf_size(buf);
 
  if (ngx_buf_in_memory(buf)) {
    unescaped = buf->pos;
  } else if (buf->in_file) {
    /* Allocate a buffer; don't bother clearing it. */
    file_buf = ngx_palloc(r->pool, unescaped_len);
    if (file_buf == NULL) {
      return NGX_ERROR;
    }
    
    /* TODO: this is blocking, but hooking into the event system
     * to read it in a non-blocking fashion seems difficult. */
    num_read = ngx_read_file( buf->file, file_buf, unescaped_len, buf->file_pos );
    if (num_read < 0) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                    "Couldn't read buffered file");
      return NGX_ERROR;
    }
    unescaped = file_buf;
  } else if (buf->last_buf) {
    /* Empty buf allowed at the end of a chain */
    return NGX_OK;
  } else {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Unexpected buffer state");
    return NGX_ERROR;
  }

  size_delta = ngx_escape_json(NULL, unescaped, unescaped_len);
  dst = json_ensure_space(j, unescaped_len + size_delta);
  if (dst == NULL) {
    return NGX_ERROR;
  }
  dst = (unsigned char *)ngx_escape_json(dst, unescaped, unescaped_len);
  j->content_length += (unescaped_len + size_delta);
  j->tail->buf->last = dst;

  if (file_buf != NULL) {
    /*
     * If the buffer was large enough, return it to the system allocator.
     * If too small, this is a no-op.
     */
    ngx_pfree(r->pool, file_buf);
    file_buf = NULL;
  }
  
  return NGX_OK;
}

/* Write the contents of the body, up to the given size, as a 
   JSON literal string. If the content is truncated,
   indicate this by a "truncated" field giving the truncated size. */
static void
ngx_akita_write_body(json_data_t *j, ngx_http_request_t *r, size_t max_size ) {
  ngx_chain_t *in;
  size_t total_size = 0; 
  static ngx_str_t body_key = ngx_string( "body" );
  static ngx_str_t truncated_key = ngx_string( "truncated" );
  
  json_write_string_literal( j, &body_key );
  json_write_char( j, ':' );
  json_write_char( j, '"' );

  if (r->request_body == NULL) {
    json_write_char( j, '"' );
    return;
  }
  
  for (in = r->request_body->bufs; in; in = in->next) {
    if (json_escape_buf(j, r, max_size, &total_size, in->buf) != NGX_OK) {
      /* TODO: a better way of signalling the error */
      j->oom = 1;
      return;
    }    
  }

  json_write_char( j, '"' );

  if (total_size > max_size) {
    json_write_char(j, ',');
    json_write_uint_property(j, &truncated_key, total_size);
  }
}

/* Set the input (request body) content size on a request. */
static ngx_int_t
ngx_akita_set_request_size(ngx_http_request_t *r, ngx_uint_t content_length) {
  ngx_str_t content_length_str;
  ngx_table_elt_t *header;

  /* 
   * "Proxy module expects the header to has [sic] a lowercased key value"
   * https://www.nginx.com/resources/wiki/start/topics/examples/headers_management/
   * but this doesnt work.
   */
  static ngx_str_t content_length_key = ngx_string("Content-Length");  

  content_length_str.data = ngx_pcalloc( r->pool, 20 );
  if (content_length_str.data == NULL) {
    return NGX_ERROR;
  }
  content_length_str.len =
    ngx_snprintf( content_length_str.data, 20, "%d", content_length ) -
    content_length_str.data;

  header = ngx_list_push(&r->headers_in.headers);
  if (header == NULL) {
    return NGX_ERROR;
  }
  header->key = content_length_key;
  header->value = content_length_str;
  header->hash = 1;
  
  r->headers_in.content_length = header;
  r->headers_in.content_length_n = content_length;
  return NGX_OK;
}

/* Set the content-type header to application/json */
static ngx_int_t
ngx_akita_set_json_content_type(ngx_http_request_t *r) {
  ngx_table_elt_t *header;
  static ngx_str_t content_type_key = ngx_string("Content-Type");  
  static ngx_str_t content_type_val = ngx_string("application/json");  

  header = ngx_list_push(&r->headers_in.headers);
  if (header == NULL) {
    return NGX_ERROR;
  }
  header->key = content_type_key;
  header->value = content_type_val;
  header->hash = 1;

  r->headers_in.content_type = header;
  return NGX_OK;
}

/* Top-level functions */

/* Initialize the Akita client based on the current configuration. */
ngx_int_t
ngx_akita_client_init(ngx_conf_t *cf) {
  ngx_str_t name = ngx_string("request_id");

  /* Cache the index of the $request_id variable */
  ngx_request_id_index = ngx_http_get_variable_index(cf, &name);
  if (ngx_request_id_index == NGX_ERROR) {
    ngx_log_error( NGX_LOG_ERR, cf->log, 0,
                   "Can't find 'request_id` variable." );
    return NGX_ERROR;
  }

  return NGX_OK;
}

static ngx_str_t post_method = ngx_string("POST");

ngx_int_t
ngx_akita_send_request_body(ngx_http_request_t *r, ngx_str_t agent_path,
                            ngx_http_akita_ctx_t *ctx,
                            ngx_http_akita_loc_conf_t *config,
                            ngx_http_post_subrequest_t *callback) {
  json_data_t *j;
  ngx_str_t request_id;
  ngx_int_t rc;
  
  j = json_alloc( r->pool );
  if (j == NULL) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "Could not allocate JSON buffer" );
    return NGX_ERROR;
  }
  
  rc = ngx_akita_get_request_id(r, &request_id );
  if (rc != NGX_OK) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "Could not get request ID" );
    return NGX_ERROR;
  }

  json_kv_string_t string_fields[] = {
    { ngx_string( "request_id" ), request_id, 0 },  /* 0 */
    { ngx_string( "method" ), r->method_name, 0 },  /* 1 */
    { ngx_string( "path" ), r->uri, 0 },            /* 2 */
    { ngx_string( "host" ), ngx_null_string, 1 },   /* 3 */
    { ngx_string( "nginx_internal" ), ngx_string( "true" ), 1 }, /* 4 */
    { ngx_null_string, ngx_null_string, 0 },
  };
  
  /* Omit host if absent. */
  if (r->headers_in.host != NULL) {
    string_fields[3].value = r->headers_in.host->value;
    string_fields[3].omit = 0;
  }

  /* Mark internal requests to help disambiguate. An internal redirect will 
   * clear the request's context so we'll see the request twice.
   * (Re-adding the context but skipping this packet would work sometimes,
   * but not always.)
   */
  if (r->internal) {
    string_fields[4].omit = 0;
  }
  
  json_write_char( j, '{' );
  json_write_kv_strings( j, string_fields );
  json_write_char( j, ',' );

  ngx_akita_write_headers_list( j, &r->headers_in.headers );
  json_write_char( j, ',' );
    
  static ngx_str_t request_start_key = ngx_string("request_start");
  static ngx_str_t request_arrived_key = ngx_string("request_arrived");
  json_write_string_literal( j, &request_start_key );
  json_write_char( j, ':' );
  json_write_time_literal( j, &ctx->request_start );  
  json_write_char( j, ',' );
  
  json_write_string_literal( j, &request_arrived_key );
  json_write_char( j, ':' );
  json_write_time_literal( j, &ctx->request_arrived );
  json_write_char( j, ',' );
                            
  ngx_akita_write_body( j, r, config->max_body_size );
  json_write_char( j, '}' );

  if (j->oom) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "JSON body got out-of-memory" );
    return NGX_ERROR;
  }

  /* Mark end of body */
  j->tail->buf->last_buf = 1;

  return ngx_akita_send_api_call(r, agent_path, callback, config, j->chain, j->content_length);
}


/* Create a subrequest with the JSON payload, sent to the configured upstream
   with the agent_path as the HTTP path. */
static ngx_int_t
ngx_akita_send_api_call(ngx_http_request_t *r,
                        ngx_str_t agent_path,
                        ngx_http_post_subrequest_t *callback,
                        ngx_http_akita_loc_conf_t *config,
                        ngx_chain_t *body,
                        size_t content_length) {
  ngx_int_t rc;
  ngx_http_request_t *subreq;
  ngx_http_akita_ctx_t *subreq_ctx;
    
  ngx_str_t query_params = ngx_null_string;
  rc = ngx_http_subrequest( r,
                            &agent_path,
                            &query_params,
                            &subreq,
                            callback,
                            NGX_HTTP_SUBREQUEST_IN_MEMORY );
  if ( rc >= NGX_HTTP_SPECIAL_RESPONSE ) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "Subrequest return code %d", rc );
    return NGX_ERROR;
  }

  /* TODO: update schema? http protocol? */
  /* TODO: which of these actually have to be set? */
  subreq->method_name = post_method;
  subreq->method = NGX_HTTP_POST;
  
  subreq->request_body = ngx_pcalloc( r->pool,
                                  sizeof(ngx_http_request_body_t) );
  if ( subreq->request_body == NULL ) {
    return NGX_ERROR;
  }
  subreq->request_body->bufs = body;

  /* Replace the existing headers entirely. 
     TODO: what to do about failure here? It seems too late to stop the subrequest. */
  ngx_akita_clear_headers( subreq );
  if (ngx_akita_set_request_size( subreq, content_length ) != NGX_OK) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "Could not set content-length header" );    
    return NGX_ERROR;
  }
  if (ngx_akita_set_json_content_type( subreq ) != NGX_OK) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "Could not set content type header" );    
    return NGX_ERROR;
  }
  /* TODO: set Host header here as well? */

  /* Assign the subrequest to the Akita agent which was configured
   * for this location. We will find this context later
   * and send it onwards to that location.
   */
  subreq_ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_akita_ctx_t));
  if (subreq_ctx == NULL) {
    return NGX_ERROR;
  }
  subreq_ctx->subrequest_upstream = &config->upstream;
  ngx_http_set_ctx(subreq, subreq_ctx, ngx_http_akita_module);
  return NGX_OK;    
  
}

typedef struct ngx_akita_internal_header_s {
  ngx_str_t key;
  ngx_str_t value;
  ngx_flag_t omit;
} ngx_akita_internal_header_t;

ngx_int_t
ngx_akita_start_response_body(ngx_http_request_t *r, 
                              ngx_http_akita_ctx_t *ctx) {
  u_char *buf;
  json_data_t *j;
  ngx_str_t request_id;
  ngx_int_t rc;
  ngx_list_t extra_headers;
  ngx_akita_internal_header_t *int_header;
  ngx_table_elt_t *header;
  
  j = json_alloc( r->pool );
  if (j == NULL) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "Could not allocate JSON buffer" );
    return NGX_ERROR;
  }
  
  rc = ngx_akita_get_request_id(r, &request_id );
  if (rc != NGX_OK) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "Could not get request ID" );
    return NGX_ERROR;
  }

  json_kv_string_t string_fields[] = {
    { ngx_string( "request_id" ), request_id, 0 },  /* 0 */
    { ngx_null_string, ngx_null_string, 0 },
  };

  json_write_char( j, '{' );
  json_write_kv_strings( j, string_fields );
  json_write_char( j, ',' );

  static ngx_str_t response_code_key = ngx_string( "response_code" );
  json_write_uint_property(j, &response_code_key, r->headers_out.status);
  json_write_char( j, ',' );

  /* Nginx-written headers are not present, nor are the ones from 
   * the upstream response that will be overwritten?  See
   * https://forum.nginx.org/read.php?2,225317,225329#msg-225329
   * So we're going to push a bunch of extra headers onto the 
   * head of a list.
   * 
   * Status            -- n/a
   * Content-Type      -- add (not included by default)
   * Content-Length    -- add
   * Date              -- copied (should already be present)
   * Last-Modified     -- add
   * ETag              -- copied
   * Server            -- copied
   * WWW-Authenticate  -- copied
   * Location          -- rewritten (should be present?)
   * Refresh           -- rewritten
   * Set-Cookie        -- rewritten
   * Content-Disposition -- copied
   * Cache-Control     -- copied
   * Expires           -- copied
   * Accept-Ranges     -- copied
   * Content-Ranges    -- copied
   * Connection        -- ignore
   * Keep-Alive        -- ignore
   * Vary              -- copied
   * Link              -- copied
   * Transfer-Encoding -- ignore
   * Content-Encoding  -- copied
   */
  ngx_akita_internal_header_t internal_headers[] = {
    /* TODO: ngx_http_header_filter_module appends a charset to this, should we? */
    { ngx_string( "Content-Type" ), r->headers_out.content_type,
      r->headers_out.content_type.len == 0 },                 /* 0 */    
    { ngx_string( "Content-Length" ), ngx_null_string, 1 },   /* 1 */
    { ngx_string( "Last-Modified" ), ngx_null_string, 1 },    /* 2 */
    { ngx_null_string, ngx_null_string, 1 },
  };

  /* The -1 value indicates unknown length */
  if (r->headers_out.content_length_n >= 0) {
    buf = ngx_pcalloc(r->pool, 20);
    internal_headers[1].value.data = buf;
    internal_headers[1].value.len = ngx_snprintf(buf, 20, "%d", r->headers_out.content_length_n) - buf;
    internal_headers[1].omit = 0;
  }

  /* The -1 value indicates absence */
  if (r->headers_out.last_modified_time >= 0) {
    /* Leave space for a full-sized timestamp but leave the \0 off the end. */
    buf = ngx_pcalloc(r->pool, sizeof("Mon, 28 Sep 1970 06:00:00 GMT") - 1 ); 
    internal_headers[2].value.data = buf;
    internal_headers[2].value.len = ngx_http_time(buf, r->headers_out.last_modified_time) - buf;
    internal_headers[2].omit = 0;
  }

  ngx_list_init(&extra_headers, r->pool, 3, sizeof(ngx_table_elt_t));
    
  for (int_header = internal_headers; int_header->key.len > 0; int_header++ ) {
    if (!int_header->omit) {
      header = ngx_list_push(&extra_headers);
      header->key = int_header->key;
      header->value = int_header->value;
    }
  };
  
  extra_headers.last->next = &r->headers_out.headers.part;
  extra_headers.last = r->headers_out.headers.last;
    
  ngx_akita_write_headers_list( j, &extra_headers );
  json_write_char( j, ',' );
    
  static ngx_str_t response_start_key = ngx_string("response_start");
  json_write_string_literal( j, &response_start_key );
  json_write_char( j, ':' );
  json_write_time_literal( j, &ctx->response_start );
  json_write_char( j, ',' );
  
  static ngx_str_t body_key = ngx_string( "body" );
  json_write_string_literal( j, &body_key );
  json_write_char( j, ':' );
  json_write_char( j, '"' );

  if (j->oom) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "JSON body got out-of-memory" );
    return NGX_ERROR;
  }

  /* 
   * Set up context for rest of body.
   * The body filter is called even when content length is zero or
   * the response is a 204.
   */
  ctx->response_json = j;
  ctx->response_body_size = 0;

  return NGX_OK;
}

ngx_int_t
ngx_akita_append_response_body(ngx_http_request_t *r,
                               ngx_http_akita_ctx_t *ctx,
                               ngx_http_akita_loc_conf_t *config,
                               ngx_buf_t *buf) {
  ngx_int_t err;
  err = json_escape_buf(ctx->response_json, r,
                        config->max_body_size,
                        &ctx->response_body_size,
                        buf);
  if (err != NGX_OK) {
    return err;
  }
  if (ctx->response_json->oom) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "JSON body got out-of-memory" );
    return NGX_ERROR;
  }
  return NGX_OK;
}

ngx_int_t
ngx_akita_finish_response_body(ngx_http_request_t *r,
                               ngx_str_t agent_path,
                               ngx_http_akita_ctx_t *ctx,
                               ngx_http_akita_loc_conf_t *config,
                               ngx_http_post_subrequest_t *callback) {
  json_data_t *j = ctx->response_json;

  /* Finish the literal that contains the response body */
  json_write_char( j, '"' );
  json_write_char( j, ',' );

  /* Mark if the body was truncated, and its actual size */
  if (ctx->response_body_size > config->max_body_size) {
    static ngx_str_t truncated_key = ngx_string( "truncated" );
    json_write_uint_property(j, &truncated_key, ctx->response_body_size);
    json_write_char( j, ',' );
  }
  
  static ngx_str_t response_start_key = ngx_string("response_complete");
  json_write_string_literal( j, &response_start_key );
  json_write_char( j, ':' );
  json_write_time_literal( j, &ctx->response_complete );

  json_write_char( j, '}' );
  
  if (j->oom) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "JSON body got out-of-memory" );
    return NGX_ERROR;
  }
  
  /* Mark end of body */
  j->tail->buf->last_buf = 1;

  return ngx_akita_send_api_call(r, agent_path, callback, config,
                                 j->chain, j->content_length);
}

