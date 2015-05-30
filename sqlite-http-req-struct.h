#ifndef __SQLITE_HTTP_REQ_STRUCT_H__
#define __SQLITE_HTTP_REQ_STRUCT_H__
#include <event2/buffer.h>
#include <microhttpd.h>

typedef struct {
  struct evbuffer *buf;
  const char *url;
  int compress;
  struct MHD_PostProcessor* post_processor;
} sqlite_httpreq_t;

sqlite_httpreq_t *sqlite_httpreq_new(const char *url);

void sqlite_httpreq_free(sqlite_httpreq_t *sr);
#endif
