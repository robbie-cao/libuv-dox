#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "dgb.h"
#include "../libuv/include/uv.h"
#include "request-parser.h"
#include "resolve-resource.h"

#define CHECK(r, msg) if (r) {                                                 \
  log_err("%s: [%s(%d): %s]\n", msg, uv_err_name((r)), r, uv_strerror((r)));   \
  exit(1);                                                                     \
}

#define UVERR(r, msg) log_err("%s: [%s(%d): %s]\n", msg, uv_err_name((r)), r, uv_strerror((r)));

#define PORT    3000
#define BACKLOG 128

#define DEFAULT_RESPONSE \
  "HTTP/1.1 200 OK\r\n" \
  "Content-Type: text/plain\r\n" \
  "Content-Length: 12\r\n" \
  "\r\n" \
  "hello world\n"

static uv_loop_t *loop;
static uv_tcp_t server;
static int id;
static uv_buf_t default_response;

static void on_connect(uv_stream_t *server, int status);
static void alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf);
static void on_req_read(uv_stream_t *req, ssize_t nread, const uv_buf_t *buf);

static void on_parse_complete(sws_parse_req_t* parse_result);
static void on_resolve_resource(sws_resource_info_t* info);
static void on_res_write(uv_write_t* req, int status);
static void on_res_end(uv_handle_t *handle);

static void alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf) {
  buf->base = malloc(size);
  buf->len = size;
}

static void on_connect(uv_stream_t *server, int status) {
  int r;

  CHECK(status, "connecting");
  debug("connecting req");

  sws_parse_req_t *parse_req = malloc(sizeof(sws_parse_req_t));
  uv_tcp_init(loop, &parse_req->handle);

  // see: on_req_read
  parse_req->handle.data = parse_req;
  parse_req->id = id++;

  sws_req_parser_init(parse_req, on_parse_complete);

  r = uv_accept(server, (uv_stream_t*) &parse_req->handle);
  if (r) {
    log_err("error accepting connection %d", r);
    uv_close((uv_handle_t*) parse_req, NULL);
  } else {
    // read the req into the tcp socket to cause it to get parsed
    // once the headers are in we'll get called back the first time (see on_headers_complete)
    // for now we assume no body since this is just a static webserver
    uv_read_start((uv_stream_t*) parse_req, alloc_cb, on_req_read);
  }
}

static void on_req_read(uv_stream_t *tcp, ssize_t nread, const uv_buf_t *buf) {
  size_t parsed;

  if (nread == UV_EOF) {
    uv_close((uv_handle_t*) tcp, NULL);

    debug("closed req tcp connection due to unexpected EOF");
  } else if (nread > 0) {
    sws_parse_req_t *parse_req = (sws_parse_req_t*) tcp->data;

    log_info("[ %3d ] req (len %ld)", parse_req->id, nread);

    parsed = sws_req_parser_execute(parse_req, buf->base, nread);
    if (parsed < nread) {
      log_err("parsing http req");
      uv_close((uv_handle_t*) &parse_req->handle, on_res_end);
    }
  } else {
    UVERR((int) nread, "reading req req");
  }
  if (buf->base) free(buf->base);
}

static void on_parse_complete(sws_parse_req_t* parse_req) {
  debug("%s\n", sws_req_parser_result_str((sws_parse_result_t*)parse_req));

  sws_resource_info_t *resource_info;
  resource_info = (sws_resource_info_t*) malloc(sizeof(sws_resource_info_t));
  sws_resolve_resource_init(resource_info, loop);

  // keep this guy around so we can clean him up later in one shot
  resource_info->data = (void*) parse_req;

  sws_resolve_resource_start(resource_info, parse_req->url, on_resolve_resource);
}

static void on_resolve_resource(sws_resource_info_t* info) {
  sws_parse_req_t* parse_req;
  parse_req = (sws_parse_req_t*) info->data;

  if (info->result) {
    UVERR(info->result, "resolve resource");
    // TODO: send 404
  } else {
    debug("resolved %s", sws_resource_info_str(info));
    parse_req->write_req.data = (void*) parse_req;
    uv_write(&parse_req->write_req, (uv_stream_t*) &parse_req->handle, &default_response, 1, on_res_write);
  }
  free(info);
}

static void on_res_write(uv_write_t* req, int status) {
  CHECK(status, "on res write");

  // keep our parse_req around
  req->handle->data = req->data;
  uv_close((uv_handle_t*) req->handle, on_res_end);
}

static void on_res_end(uv_handle_t *handle) {
  sws_parse_req_t* parse_req = (sws_parse_req_t*) handle->data;
  log_info("[ %3d ] connection closed", parse_req->id);
  // was cleaned in on_resolve
  sws_cleanup_parse_req(parse_req);
  free(parse_req);
}

int main() {
  int r;
  default_response.base = DEFAULT_RESPONSE;
  default_response.len = strlen(default_response.base);

  id = 0;
  loop = uv_default_loop();

  struct sockaddr_in bind_addr;
  r = uv_ip4_addr("0.0.0.0", PORT, &bind_addr);
  CHECK(r, "get bind addr");

  r = uv_tcp_init(loop, &server);
  CHECK(r, "init server");

  r = uv_tcp_bind(&server, (const struct sockaddr*) &bind_addr);
  CHECK(r, "bind");

  r = uv_listen((uv_stream_t*) &server, BACKLOG, on_connect);
  log_info("listening on http://localhost:%d", PORT);

  uv_run(loop, UV_RUN_DEFAULT);
  return 0;
}
