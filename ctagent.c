#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <event2/event.h>

#include <sqlite3.h>
#include <json-c/json_tokener.h>
#include <json-c/json_object.h>
#include <curl/curl.h>

#define DD(...) fprintf(stdout, "*****:"__VA_ARGS__)
#define EE(...) fprintf(stderr, __VA_ARGS__)

static const char fake_ok_resp[] = {"{\"status\": \"OK\"}"};
static const char error_resp[] = {"{\"status\": \"error\"}"};

typedef struct {
  struct event_base *base;
  struct evconnlistener *listener;
} ct_agent_t;

typedef struct {
  CURL *handle;
  json_object *obj;
  struct bufferevent *bev;
  struct json_tokener *tok;
}ct_request_t;

static void client_write_cb(struct bufferevent *bev, void *user_data) {
  struct evbuffer *out = bufferevent_get_output(bev);
  DD("out.length = %d\n", (int)evbuffer_get_length(out));
}

static size_t resp_parse_callback(void *contents, size_t length, size_t nmemb, void *userdata) {
  json_object *obj = NULL;
  ct_request_t *cr = (ct_request_t *)userdata;
  enum json_tokener_error jerr;
  size_t tsize = length * nmemb;

  cr->obj = json_tokener_parse_ex(cr->tok, contents, tsize);
  jerr = json_tokener_get_error(cr->tok);
  DD("cr.obj = %p, continue = %d\n", cr->obj, jerr == json_tokener_continue);

  return tsize;
}

static json_object* ct_request_process(char *sbuf, struct bufferevent *bev) {
  ct_request_t cr;
  CURLcode res;

  cr.handle = curl_easy_init();
  curl_easy_setopt(cr.handle, CURLOPT_VERBOSE, 0L);
  curl_easy_setopt(cr.handle, CURLOPT_URL, sbuf);
  curl_easy_setopt(cr.handle, CURLOPT_WRITEFUNCTION, resp_parse_callback);
  curl_easy_setopt(cr.handle, CURLOPT_WRITEDATA, (void *)&cr);

  cr.bev = bev;
  cr.tok = json_tokener_new();

  res = curl_easy_perform(cr.handle);
  if(res != CURLE_OK) {
    DD("curl perform error: %s\n", curl_easy_strerror(res));
    bufferevent_write(bev, error_resp, sizeof(error_resp)-1);
  } else if(cr.obj != NULL) {
    DD("resp json object parsed\n");
    bufferevent_write(bev, fake_ok_resp, sizeof(fake_ok_resp)-1);
    //EE("%s\n", json_object_get_string(cr.obj));
  }

  json_tokener_free(cr.tok);
  curl_easy_cleanup(cr.handle);

  return cr.obj;
}

static void client_read_cb(struct bufferevent *bev, void *user_data) {
  struct evbuffer *in = bufferevent_get_input(bev);
  int nsize = evbuffer_get_length(in);
  if(nsize > 0) {
    char *sbuf = calloc(nsize+1, sizeof(char));
    evbuffer_remove(in, sbuf, nsize);
    DD("client read nsize = %d, %s\n", nsize, sbuf);

    if(strncmp(sbuf, "quit", 4) == 0) {
      bufferevent_free(bev);
    } else {
      if(strncmp(sbuf, "http", 4) == 0) {
        json_object *obj = NULL;
        *(sbuf+nsize-2) = '\0';
        obj = ct_request_process(sbuf, bev);
      } else {
        *(sbuf+nsize-2) = '*'; /** append'*' to end of echo message in telnet*/
        bufferevent_write(bev, sbuf, nsize);
      }
    }

    free(sbuf);
  }
}

static void client_event_cb(struct bufferevent *bev, short events, void *user_data) {
  DD("client events %x\n", events);
  if(events & BEV_EVENT_EOF) {
    DD("BEV_EVENT_EOF event\n");
  } else if(events & BEV_EVENT_ERROR) {
    DD("BEV_EVENT_ERROR: %s\n", strerror(errno));
  }
  bufferevent_free(bev);
}

static void ct_agent_listener_cb(struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *sa, int socklen, void *user_data) {
  ct_agent_t *obj = (ct_agent_t *)user_data;
  struct bufferevent *bev = NULL;

  bev = bufferevent_socket_new(obj->base, fd, BEV_OPT_CLOSE_ON_FREE);
  if(!bev) {
    DD("new client accept failed\n");
    event_base_loopbreak(obj->base);
    return;
  }

  bufferevent_setcb(bev, client_read_cb, client_write_cb, client_event_cb, obj);
  bufferevent_enable(bev, EV_WRITE);
  bufferevent_enable(bev, EV_READ);
}

ct_agent_t* ct_agent_new(struct event_base *base, int port) {
  struct sockaddr_in sin;
  ct_agent_t *obj = calloc(1, sizeof(ct_agent_t));

  obj->base = base;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);

  obj->listener = evconnlistener_new_bind(base, ct_agent_listener_cb, (void *)obj, LEV_OPT_REUSEABLE|LEV_OPT_CLOSE_ON_FREE, -1, (struct sockaddr *)&sin, sizeof(sin));
  if(!obj->listener) {
    DD("evconnlistener_new_bind failed\n");
    return NULL;
  }

  return obj;
}

void ct_agent_free(ct_agent_t *agent) {
  evconnlistener_free(agent->listener);
}

int main(int argc, char *argv[]) {
  char *port = argv[1];
  int nport = 8899;
  struct event_base *base;

  if(port) {
    nport = atoi(port);
  }

  ct_agent_t *agent = NULL;

  curl_global_init(CURL_GLOBAL_ALL);
  base = event_base_new();
  agent = ct_agent_new(base, nport);

  DD("agent %p run @ %d\n", agent, nport);
  event_base_dispatch(base);

  ct_agent_free(agent);
  event_base_free(base);
  curl_global_cleanup();
  return 0;
}
