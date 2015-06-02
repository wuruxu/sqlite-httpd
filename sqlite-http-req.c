#include <stdlib.h>
#include "sqlite-http-req-struct.h"

sqlite_httpreq_t *sqlite_httpreq_new(const char *url) {
  sqlite_httpreq_t *sr = calloc(1, sizeof(sqlite_httpreq_t));

  sr->url = url;
  sr->buf = evbuffer_new();
  sr->compress = 0;

  return sr;
}

void sqlite_httpreq_set_compress(sqlite_httpreq_t *sr, int compress) {
  sr->compress = compress;
}

void sqlite_httpreq_free(sqlite_httpreq_t *sr) {
  if(sr != NULL) {
    evbuffer_free(sr->buf);
    free(sr);
  }
}


