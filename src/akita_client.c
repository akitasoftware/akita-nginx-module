/*
 * Copyright (C) 2023 Akita Software
 */

#include "ngx_http_akita_module.h"
#include "akita_client.h"

static ngx_int_t
akita_get_request_id(ngx_http_request_t *r, ngx_str_t *dest);


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
/* TODO: static void json_write_int_literal( json_data_t *buf, ngx_uint_t n ); */

/* A key and string value to write into a JSON object */
typedef struct json_kv_string_s {
  ngx_str_t key;
  ngx_str_t value;
  ngx_int_t omit;  /* Skip this key in the array */
} json_kv_string_t;

static void json_write_kv_strings( json_data_t *buf, json_kv_string_t *kvs );


static ngx_uint_t json_initial_size = 4096;

/* Allocate a new buffer for JSON */
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

/* Write a single character to the JSON buffer */
static void json_write_char( json_data_t *j, unsigned char c ) {
  unsigned char *p = json_ensure_space( j, 1 );
  if (p == NULL) {
    return;
  }
  *p = c;
  j->content_length++;
  j->tail->buf->last++;
}

/* Write a properly-escaped string literal to the JSON buffer, including "" */
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
  dst = (unsigned char *)ngx_escape_json( dst, str->data, str->len );
  *dst++ = '"';
  j->content_length += sz;
  j->tail->buf->last = dst;
}

/*
 * Output an array of key/value pairs. The last pair should be signalled with
 * an null key. Commas appear between pairs but not after the last.
 */
static void json_write_kv_strings(json_data_t *j, json_kv_string_t *kv) {
  ngx_uint_t need_comma = 0;
  
  while (kv->key.len > 0) {
    if (need_comma) {
      json_write_char(j, ',');      
    }

    if (kv->omit) {
      need_comma = 0;
    } else {
      json_write_string_literal(j, &(kv->key));
      json_write_char(j, ':');
      json_write_string_literal(j, &(kv->value));
      need_comma = 1;
    }

    kv++;
  }
}

/*
 * Output a timestamp as a string literal.  We use RFC3339 format with 
 * microsecond precision and UTC time zone. Note that the ngx_request_t 
 * timestamp is in seconds and milliseconds.
 */
static void json_write_time_literal(json_data_t *j, struct timeval *tv) {
  static ngx_str_t format = ngx_string("\"2006-01-02T15:04:05.999999Z\"");
  ngx_tm_t tm;

  ngx_gmtime(tv->tv_sec, &tm);  
  unsigned char *p = json_ensure_space(j, format.len);
  if (p == NULL) {
    return;
  }
  ngx_sprintf(p, "\"%4d-%02d-%02dT%02d:%02d:%02d.%06dZ\"",
              tm.ngx_tm_year, tm.ngx_tm_mon,
              tm.ngx_tm_mday, tm.ngx_tm_hour,
              tm.ngx_tm_min, tm.ngx_tm_sec,
              tv->tv_usec);
  j->content_length += format.len;
  j->tail->buf->last += format.len;    
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
 * Get the request ID as a string.
 * TODO: for ngx prior to 1.11.0, we need to use the connection and
 * connection requests to generate an ID.
 */
static
ngx_int_t
akita_get_request_id(ngx_http_request_t *r, ngx_str_t *dest) {
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
ngx_akita_write_request_headers(json_data_t *j, ngx_http_request_t *r ) {
  ngx_list_part_t *header_part;
  ngx_table_elt_t *headers;
  ngx_uint_t i = 0;
  ngx_uint_t need_comma = 0;
  
  static ngx_str_t headers_key = ngx_string( "headers" );
  static ngx_str_t header_key = ngx_string( "header" );
  static ngx_str_t value_key = ngx_string( "value" );
  json_write_string_literal(j, &headers_key);
  json_write_char(j, ':' );
  json_write_char(j, '[' );  
  for (header_part = &(r->headers_in.headers.part); header_part; header_part = header_part->next) {
    headers = header_part->elts;
    for (i = 0; i < header_part->nelts; i++) {
      if (need_comma) {
        json_write_char(j, ',');
      }
      json_write_char(j, '{');
      json_write_string_literal(j, &header_key);
      json_write_char(j, ':' );
      json_write_string_literal(j, &headers[i].key);
      json_write_char(j, ',' );      
      json_write_string_literal(j, &value_key);
      json_write_char(j, ':' );
      json_write_string_literal(j, &headers[i].value);
      json_write_char(j, '}');
      need_comma = 1;
    }
  }
  json_write_char(j, ']' );  
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

  content_length_str.data = ngx_pcalloc( r->connection->pool, 20 );
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
                            ngx_http_post_subrequest_t *callback) {
  json_data_t *j;
  ngx_str_t request_id;
  ngx_int_t rc;
  ngx_http_request_t *subreq;
  
  j = json_alloc( r->connection->pool );

  rc = akita_get_request_id(r, &request_id );
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
    { ngx_null_string, ngx_null_string, 0 },
  };
  
  /* Omit host if absent. */
  if (r->headers_in.host != NULL) {
    string_fields[3].value = r->headers_in.host->value;
    string_fields[3].omit = 0;
  }

  json_write_char( j, '{' );
  json_write_kv_strings( j, string_fields );
  json_write_char( j, ',' );

  ngx_akita_write_request_headers( j, r );
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
                            
  json_write_char( j, '}' );

  if (j->oom) {
    ngx_log_error( NGX_LOG_ERR, r->connection->log, 0,
                   "JSON body got out-of-memory" );
    return NGX_ERROR;
  }

  /* Mark end of body */
  j->tail->buf->last_buf = 1;

  /* Prepare a subrequest with the JSON payload, sent to an internal path. */
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
  
  subreq->request_body = ngx_pcalloc( r->connection->pool,
                                  sizeof(ngx_http_request_body_t) );
  if ( subreq->request_body == NULL ) {
    return NGX_ERROR;
  }
  subreq->request_body->bufs = j->chain;

  /* Replace the existing headers entirely. 
     TODO: what to do about failure here? It seems to late to stop the subrequest. */
  ngx_akita_clear_headers( subreq );
  if (ngx_akita_set_request_size( subreq, j->content_length ) != NGX_OK) {
    return NGX_ERROR;
  }
  if (ngx_akita_set_json_content_type( subreq ) != NGX_OK) {
    return NGX_ERROR;
  }
  
  return NGX_OK;    
}

