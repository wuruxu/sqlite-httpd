#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/event_compat.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <json-c/json_object.h>

#include "internal-debug.h"
#include "sqlite-http-req-struct.h"
#include "sqlite-json-info.h"

#define  TAG __FILE__

static const char fake_ok_resp[] = {"{\"status\": \"OK\"}"};
static const char outofmem_resp[] = {"{\"status\": \"error = Out Of Memory\"}"};
static const char ctype_resp[] = {"{\"status\": \" make sure content-type set as 'application/x-www-form-urlencoded'\"}"};

typedef struct {
  struct event_base *base;
  struct event signal_int;
  struct MHD_Daemon *httpd;
  sqlite3 *db;
} sqlitehttpd_t;

static sqlitehttpd_t shd;
static unsigned int reqcnt = 0;

static void signal_event(int fd, short event, void *arg) {
    sqlitehttpd_t *dp = (sqlitehttpd_t *) arg;
    int signum;

    DD("%s: got signal %d\n", __func__, EVENT_SIGNAL(&dp->signal_int));
    signum = EVENT_SIGNAL(&dp->signal_int);
    switch(signum)
    {
    case SIGINT:
        event_base_loopexit(dp->base, NULL);
        DD("event_base_loopexit called\n");
        break;
    }
}

int post_iterator (void *cls, enum MHD_ValueKind kind, const char *key, const char *filename, const char *content_type, const char *transfer_encoding, const char *data, uint64_t off, size_t size) {
    return MHD_YES;
}

static int parse_http_request_header(void *cls, enum MHD_ValueKind kind, const char *key, const char *value) {
  sqlite_httpreq_t *sr = (sqlite_httpreq_t *)cls;

  if(sr == NULL) 
    return MHD_NO;

  if(strcmp(key, "Accept-Encoding") == 0) {
    if(strcmp(value, "gzip, deflate") == 0) {
      sr->compress = 1;
    } else {
      sr->compress = 0;
    }
  }

  if(strcmp(sr->url, "/sqlite") == 0) {
  }

  return MHD_YES;
}

static int generate_json_from_stmt(char* buf, size_t max, sqlite_json_info_t* sji) {
  int i = 0;
  int off = 0;
  sqlite3_stmt *stmt = sji->stmt;
  char *ptr = buf;

  if(sji->rstart == 0) {
    *ptr = ',', *(ptr+1) = '{', off = 2;
  } else {
    sqlite_json_info_set_rstart(sji, 0);
    *ptr = '{', off = 1;
  }

  for(; i < sji->ncol; i++) {
    int nbytes = 0;
    int type = sji->coltypes[i];
    //DD("--buf= %s\n", buf);
    buf = ptr + off;
    switch(type) {
      case SQLITE_INTEGER:
      //DD("SQLITE_INTEGER: %s:%d\n",  sji->colnames[i], sqlite3_column_int(stmt, i));
      nbytes = sprintf(buf, "\"%s\":%d,", sji->colnames[i], sqlite3_column_int(stmt, i));
      break;

      case SQLITE_FLOAT:
      //DD("SQLITE_FLOAT: %s:%f\n",  sji->colnames[i], sqlite3_column_double(stmt, i));
      nbytes = sprintf(buf, "\"%s\":%f,", sji->colnames[i], sqlite3_column_double(stmt, i));
      break;

      case SQLITE_BLOB:
      nbytes = sprintf(buf, "\"%s\":\"BLOB\",", sji->colnames[i]);
      break;

      case SQLITE_NULL:
      nbytes = sprintf(buf, "\"%s\":null,", sji->colnames[i]);
      break;

      case SQLITE3_TEXT: {
      const char *text = sqlite3_column_text(stmt, i);
      if(! text) {
        text = "null";
      }
      json_object *obj = json_object_new_string(text);
      nbytes = sprintf(buf, "\"%s\":%s,", sji->colnames[i], json_object_to_json_string(obj));
      json_object_put(obj);
      break;
      }
    }
    off += nbytes;
  }
  *(ptr+off-1) = '}';

  //DD("ROW: %s\n", ptr);
  return off;
}

static ssize_t sqlite_row_read_callback(void *cls, uint64_t pos, char* buf, size_t max) {
  sqlite_json_info_t *sji = (sqlite_json_info_t *) cls;
  int ret, nbyte = 0;

  //DD("sqlite_row_read_callback: pos = %d, buf = %x, max = %zu\n", pos, (unsigned int)buf, max);
  if(sji->eos == 1) {
    return MHD_CONTENT_READER_END_OF_STREAM;
  }

  ret = sqlite3_step(sji->stmt);
  //buf = buf + pos;
  if(ret == SQLITE_ROW) {
    if(!sqlite_json_info_initialized(sji)) {
       nbyte = sprintf(buf, "{\"jsondata\":[");
       buf = buf + nbyte;
       max -= nbyte;
       sqlite_json_info_init(sji);
    }

    nbyte += generate_json_from_stmt(buf, max, sji);
    return nbyte;
  } else if(ret == SQLITE_DONE) {
    sqlite_json_info_set_eos(sji, 1);
    nbyte = sprintf(buf, "]}");
    return nbyte;
  } else {
    return MHD_CONTENT_READER_END_WITH_ERROR;
  }

  return 0;
}

static struct MHD_Response* process_sqlite_request(sqlite_httpreq_t *sr) {
  struct MHD_Response *resp = NULL;
  int len = evbuffer_get_length(sr->buf);
  char *sbuf = calloc(len, sizeof(char));
  struct sqlite3_stmt* stmt = NULL;
  int ret = 0;
  struct evbuffer *jsonbuf = NULL;
  sqlite_json_info_t *sji = NULL;

  if(sbuf != NULL) {
    evbuffer_remove(sr->buf, sbuf, len-1);
    
    DD("sbuf = %s\n\n", sbuf);
    ret = sqlite3_prepare_v2(shd.db, sbuf, len, &stmt, NULL);
    free(sbuf);
    sji = sqlite_json_info_new(stmt);
    resp = MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, 1024*16, sqlite_row_read_callback, sji, sqlite_json_info_free);
    if(resp == NULL) {
      resp = MHD_create_response_from_buffer(sizeof(fake_ok_resp)-1, (void *)fake_ok_resp, MHD_RESPMEM_PERSISTENT);
    }
  } else {
    resp = MHD_create_response_from_buffer(sizeof(outofmem_resp)-1, (void *)outofmem_resp, MHD_RESPMEM_PERSISTENT);
  }

  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");

  return resp;
}

static int answer_http_connection (void* cls, struct MHD_Connection* connection, const char* url, const char* method, const char* version, const char* upload_data, size_t* upload_data_size, void** con_cls) {
  struct MHD_Response *resp = NULL;

  if(strcmp(method, MHD_HTTP_METHOD_POST) == 0) {
    if(strcmp(url, "/sqlite") == 0) {
      sqlite_httpreq_t *sr = (sqlite_httpreq_t *) *con_cls;

      //DD("POST url %s, upload_data = %x, data_size = %d, sr = %x\n", url, (int)upload_data, (int)*upload_data_size, (unsigned int)sr);
      if(sr == NULL) {
        reqcnt++;
        DD("serve reqest count id %d\n", reqcnt);
        sr = sqlite_httpreq_new(url);
        sr->post_processor = MHD_create_post_processor(connection, 64*1024, post_iterator, NULL);
        MHD_get_connection_values(connection, MHD_HEADER_KIND, &parse_http_request_header, sr);
        if(!sr->post_processor) {
          DD("Could not create post_processor\n");
          return MHD_NO;
        }

        *con_cls = sr;
        return MHD_YES;
      }

      MHD_post_process(sr->post_processor, upload_data, *upload_data_size);

      if(*upload_data_size > 0) {
        //DD("evbuffer size = %zu, *upload_data_size = %zu\n", evbuffer_get_length(sr->buf), *upload_data_size);
        //DD("step 002, upload_data_size = %d, sr->buf = %x , sr=%x\n", *upload_data_size, sr->buf, sr);
        if(evbuffer_add(sr->buf, upload_data, *upload_data_size) == 0) {
          //DD("evbuffer total size = %d, upload_data_size = %d\n", evbuffer_get_length(sr->buf), *upload_data_size);
          *upload_data_size = 0;
          return MHD_YES;
        } else {
          return MHD_YES;
        }
      }

      resp = process_sqlite_request(sr);
      //MHD_queue_response(connection, MHD_HTTP_OK, resp);
      //MHD_destroy_response(resp);
      sqlite_httpreq_free(sr);
    }/** url=/sqlite */
  } else if (strcmp(method, MHD_HTTP_METHOD_GET) == 0) {
    resp = MHD_create_response_from_buffer(sizeof(fake_ok_resp)-1, (void *)fake_ok_resp, MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "text/html");
  }

  if(resp) {
    MHD_queue_response(connection, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
  }

  return MHD_YES;
}

int main(int argc, char *argv[]) {
  char *dbfile = argv[1];
  char *port = argv[2];
  int ret = 0;
  int nport = atoi(port);

  shd.base = event_base_new();
  ret = sqlite3_open(dbfile, &shd.db);
  if(ret != 0) {
    DD("open sqlite database failed\n");
    return -1;
  }

  event_set(&shd.signal_int, SIGINT, EV_SIGNAL|EV_PERSIST, signal_event, &shd);
  event_base_set(shd.base, &shd.signal_int);
  event_add(&shd.signal_int, NULL);

  shd.httpd = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION|MHD_USE_DEBUG, nport, NULL, NULL, &answer_http_connection, NULL, MHD_OPTION_END);

  if(! shd.httpd) {
    DD("Could not start up daemon @ %d\n", nport);
    return -1;
  } else {
    DD("sqlite httpd started @ %d\n", nport);
  }

  event_base_dispatch(shd.base);

  MHD_stop_daemon(shd.httpd);

  sqlite3_close(shd.db);
  event_base_free(shd.base);

  return 0;
}


