#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/event_compat.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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
      DD("http compress support enable\n");
      sqlite_httpreq_set_compress(sr, 1);
    } else {
      sqlite_httpreq_set_compress(sr, 0);
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

  //DD("buf.addr = %x\n", buf);
  if(sji->rstart == 0) {
    *ptr = ',', *(ptr+1) = '{', off = 2;
  } else {
    sqlite_json_info_set_rstart(sji, 0);
    *ptr = '{', off = 1;
  }

  for(buf=ptr+off; i < sji->ncol; i++) {
    int nbytes = 0;
    int type = sji->coltypes[i];
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
      } else {
      }
      json_object *obj = json_object_new_string(text);
      nbytes = sprintf(buf, "\"%s\":%s,", sji->colnames[i], json_object_to_json_string(obj));
      json_object_put(obj);
      break;
      }
    }
    off += nbytes;
    buf += nbytes;
  }
  *(ptr+off-1) = '}';

  //DD("ROW %x: %s\n", ptr+off-1, ptr);
  return off;
}

static ssize_t sqlite_row_read_callback(void *cls, uint64_t pos, char* buf, size_t max) {
  sqlite_json_info_t *sji = (sqlite_json_info_t *) cls;
  int ret, nbyte = 0, err;
  char *respbuf = NULL;
  int outsize = 0;
  int _max = max;

  if(sji->eos == EOS_TRUE) {
    return MHD_CONTENT_READER_END_OF_STREAM;
  }/**sji->eos == 1*/

  //DD("sqlite_row_read_callback: pos = %d, buf = %x, max = %zu\n", pos, (unsigned int)buf, max);

  if(sji->compress) {
    sji->zs.next_out = buf;
    sji->zs.avail_out = _max;
    if(sji->zs.avail_in > 0 || sji->eos == EOS_GZING) {
      goto buf_compress;
    }
  }

  do {
    int rowcnt = 0;
    max = _max, outsize = 0, respbuf = sji->compress ? sji->gzbuf: buf;

    DD("S1-deflate out: ai %d, ao %d\n", sji->zs.avail_in, sji->zs.avail_out);

    while(outsize < max) {
      nbyte = 0;
      ret = sqlite3_step(sji->stmt);
      //buf = buf + pos;
      //DD("respbuf=%s.\n", buf);
      if(ret == SQLITE_ROW) {
        rowcnt++;
        if(!sqlite_json_info_initialized(sji)) {
           nbyte = sprintf(respbuf, "{\"jsondata\":[");
           respbuf += nbyte;
           outsize += nbyte;
           //max -= nbyte;
           sqlite_json_info_init(sji);
        }

        nbyte = generate_json_from_stmt(respbuf, _max-outsize, sji);
        respbuf += nbyte;
        outsize += nbyte;
        max = _max - nbyte - nbyte;
        //max -= nbyte;
      } else if(ret == SQLITE_DONE) {
        if(rowcnt == 0) {
          nbyte = sprintf(respbuf, "{\"jsondata\":[\"status\": \"SQLITE_DONE\"]}");
        } else {
          nbyte = sprintf(respbuf, "]}");
        }
        sqlite_json_info_set_eos(sji, EOS_TRUE);
        outsize += nbyte;
        break;
      } else {
        json_object *obj = NULL;
        if(sji->compress) {
          DD("deflateEnd\n");
          deflateEnd(&sji->zs);
        }
        obj = json_object_new_string(sqlite3_errmsg(sqlite3_db_handle(sji->stmt)));
        nbyte = sprintf(respbuf, "{\"jsondata\":[\"errmsg\": %s]}", json_object_to_json_string(obj));
        json_object_put(obj);
        sqlite_json_info_set_eos(sji, EOS_TRUE);
        outsize += nbyte;
        break;
      }
    } /**nbyte < max*/

    //DD("rowcnt = %d, outsize = %d, outbuf = %s\n", rowcnt, outsize, buf);

    if(sji->compress) {
        //DD("S2-deflate out: outsize= %d, _max %d , ai %d, ao %d\n", outsize, _max,  sji->zs.avail_in, sji->zs.avail_out);
        sji->zs.next_in = sji->gzbuf;
        sji->zs.avail_in = outsize;

  buf_compress:
        //DD("S3-deflate out: ni %p, no %p , ai %d, ao %d\n", sji->zs.next_in, sji->zs.next_out,  sji->zs.avail_in, sji->zs.avail_out);
        while(sji->zs.avail_in != 0) {
          //DD("W01-deflate out: ai %d, ao %d, msg=%s\n", sji->zs.avail_in, sji->zs.avail_out, sji->zs.msg);
          err = deflate(&sji->zs, Z_NO_FLUSH);
          if(Z_OK != err && Z_STREAM_END != err && Z_BUF_ERROR != err) {
            DD("deflate != Z_OK, err = %d\n", err);
            return err;
          }

          //DD("W02-deflate out: ni %p, no %p , ai %d, ao %d, msg=%s, err = %x\n", sji->zs.next_in, sji->zs.next_out,  sji->zs.avail_in, sji->zs.avail_out, sji->zs.msg, err);
          if(sji->zs.next_in) {
            if(sji->zs.avail_in == 0) {
              sji->zs.next_in = NULL;
              break;
            }
          }
          if(sji->zs.avail_out == 0) {
            //DD("W03-deflate out: ai %d, ao %d, msg=%s\n", sji->zs.avail_in, sji->zs.avail_out, sji->zs.msg);
            break;
          }
        }

        if(sji->eos == EOS_TRUE || sji->eos == EOS_GZING) {
          sqlite_json_info_set_eos(sji, EOS_GZING);
          do {
            err = deflate(&sji->zs, Z_FINISH);
            //DD("deflate.Z_FINISH errcode = %d, ai %d, ao %d\n", err, sji->zs.avail_in, sji->zs.avail_out);
            if(err != Z_OK) {
              if(err != Z_STREAM_END) return MHD_CONTENT_READER_END_WITH_ERROR;
            } else {
              if(sji->zs.avail_out == 0) {
                return _max;
              }
            }
          } while(err != Z_STREAM_END);
          sqlite_json_info_set_eos(sji, EOS_TRUE);

          if(Z_OK != deflateEnd(&sji->zs)) {
            //DD("deflateEnd: gz compress failed\n");
          }
          //DD("sji->eos is ready, Z_FINISH.err = %x, ai %d, ao %d\n", err,  sji->zs.avail_in, sji->zs.avail_out);
          break;
        }
    } /*compress = 1*/

    DD("S4-deflate out: ai %d, ao %d\n", sji->zs.avail_in, sji->zs.avail_out);
  } while(sji->compress && sji->zs.avail_out > 0);

  outsize = sji->compress ? _max - sji->zs.avail_out : outsize;
  //DD("END-callback deflate out: ai %d, ao %d, outsize = %d\n\n", sji->zs.avail_in, sji->zs.avail_out, outsize);

  return outsize;
}

static struct MHD_Response* process_sqlite_request(sqlite_httpreq_t *sr) {
  struct MHD_Response *resp = NULL;
  int len = evbuffer_get_length(sr->buf);
  char *sbuf = calloc(len+1, sizeof(char));
  struct sqlite3_stmt* stmt = NULL;
  int ret = 0;
  struct evbuffer *jsonbuf = NULL;
  sqlite_json_info_t *sji = NULL;

  if(sbuf != NULL) {
    evbuffer_remove(sr->buf, sbuf, len);
    DD("sbuf = %s\n\n", sbuf);
    ret = sqlite3_prepare_v2(shd.db, sbuf, len, &stmt, NULL);
    free(sbuf);
    if(stmt == NULL) {
      char _respbuf[2048] = {0};
      int nbyte = 0;
      json_object *obj = NULL; //json_object_new_string(text);
      sqlite_httpreq_set_compress(sr, 0);
      obj = json_object_new_string(sqlite3_errmsg(shd.db));
      nbyte = sprintf(_respbuf, "{\"jsondata\":[\"errmsg\": %s]}", json_object_to_json_string(obj));
      json_object_put(obj);
      resp = MHD_create_response_from_buffer(nbyte, (void *)_respbuf, MHD_RESPMEM_MUST_COPY);
    } else {
      sji = sqlite_json_info_new(stmt, sr->compress);
      if(sji) {
        resp = MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, GZ_CACHE_SIZE, sqlite_row_read_callback, sji, sqlite_json_info_free);
      } else {
        resp = MHD_create_response_from_buffer(sizeof(outofmem_resp)-1, (void *)outofmem_resp, MHD_RESPMEM_PERSISTENT);
      }

      if(resp == NULL) {
        resp = MHD_create_response_from_buffer(sizeof(fake_ok_resp)-1, (void *)fake_ok_resp, MHD_RESPMEM_PERSISTENT);
      }
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

      DD("POST url %s, upload_data = %p, data_size = %d, sr = %p\n", url, upload_data, (int)*upload_data_size, sr);
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
      if(resp && sr->compress) {
        MHD_add_response_header(resp, "Content-Encoding", "gzip");
      }
      //MHD_queue_response(connection, MHD_HTTP_OK, resp);
      //MHD_destroy_response(resp);
      MHD_destroy_post_processor(sr->post_processor);
      sqlite_httpreq_free(sr);
      *con_cls = NULL;
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

static void print_usage() {
  printf("\tsqlitehttpd: a tiny http server for sqlite3\n");
  printf("\t\t -p: set http server port.(default 8888)\n");
  printf("\t\t -o: specify the sqlite database file.\n");
}

int main(int argc, char *argv[]) {
  char *dbfile = NULL;
  char *port = NULL;
  int ret = 0, opt;
  int nport = 8888;

  while((opt = getopt(argc, argv, "p:o:h")) != -1) {
    switch(opt) {
    case 'p':
      nport = atoi(optarg);
      break;
    case 'o':
      dbfile = optarg;
      break;
    case 'h':
      print_usage();
      return 0;
    }
  }

  if(dbfile == NULL) {
    print_usage();
    return 0;
  }

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


