/*
 *
 * Copyright 2016 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "src/core/lib/iomgr/port.h"

#ifdef GRPC_UV
#include <limits.h>
#include <string.h>

#include <uv.h>

#include <grpc/slice_buffer.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>

#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/iomgr_custom.h"
#include "src/core/lib/iomgr/network_status_tracker.h"
#include "src/core/lib/iomgr/resource_quota.h"
#include "src/core/lib/iomgr/tcp_custom.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"
#include "src/core/lib/support/string.h"

grpc_tracer_flag grpc_tcp_trace = GRPC_TRACER_INITIALIZER(true, "tcp");

typedef struct {
  uv_write_t write_req;
  uv_shutdown_t shutdown_req;
  uv_tcp_t *handle;
  uv_buf_t *write_buffers;
  size_t write_len;

  char* read_buf;
  size_t read_len;
} uv_socket;

static void uv_destroy(grpc_endpoint* ep) {
  uv_socket* s = grpc_endpoint_get_socket(ep);
  gpr_free(s->handle);
  gpr_free(s);
}

static void alloc_uv_buf(uv_handle_t *handle, size_t suggested_size,
                         uv_buf_t *buf) {
  uv_socket *s = (uv_socket*)grpc_endpoint_get_socket((grpc_endpoint*)handle->data);
  (void)suggested_size;
  buf->base = s->read_buf;
  buf->len = s->read_len;
}

static void uv_read_callback(uv_stream_t *stream, ssize_t nread,
                          const uv_buf_t *buf) {
  grpc_error *error = GRPC_ERROR_NONE;
  if (nread == 0) {
    // Nothing happened. Wait for the next callback
    return;
  }
  // TODO(murgatroid99): figure out what the return value here means
  uv_read_stop(stream);
  if (nread == UV_EOF) {
    error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("EOF");
  } else if (nread > 0) {
    error = GRPC_ERROR_NONE;
  }
  grpc_custom_read_callback((grpc_endpoint*)stream->data, (size_t)nread, error);
}


//TODO DO SOMETHING ABOUT THE UNREF
static void uv_close_callback(uv_handle_t *handle) {}

static void uv_async_read(grpc_endpoint *ep, char* buffer, size_t length) {
  uv_socket *s = (uv_socket*) grpc_endpoint_get_socket(ep);
  int status;
  grpc_error* error;
  s->read_buf = buffer;
  s->read_len = length;
  // TODO(murgatroid99): figure out what the return value here means
  status =
      uv_read_start((uv_stream_t *)s->handle, (uv_alloc_cb)alloc_uv_buf, (uv_read_cb)uv_read_callback);
  if (status != 0) {
    error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("TCP Read failed at start");
    //error =
    //    grpc_error_set_str(error, GRPC_ERROR_STR_OS_ERROR,
    //                       grpc_slice_from_static_string(uv_strerror((uv_err_t)status)));
    grpc_custom_read_callback(ep, 0, error);
  }
}

static void uv_write_callback(uv_write_t *req, int status) {
  grpc_endpoint* ep = (grpc_endpoint*) req->data;
  uv_socket *s = (uv_socket*) grpc_endpoint_get_socket(ep);
  grpc_error *error;
  if (status == 0) {
    error = GRPC_ERROR_NONE;
  } else {
    error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("TCP Write failed");
  }
  gpr_free(s->write_buffers);
  grpc_custom_write_callback((grpc_endpoint*) s->handle->data, s->write_len, error);
}

void uv_endpoint_write(grpc_endpoint* endpoint, char* buffer, size_t length) {
  uv_socket* socket = (uv_socket*) grpc_endpoint_get_socket(endpoint);
  uv_buf_t *buffers;
  uv_write_t *write_req;

  buffers = gpr_malloc(sizeof(uv_buf_t));
  buffers[0].base = buffer;
  buffers[0].len = length;

  socket->write_buffers = buffers;
  socket->write_len = length;
  write_req = &socket->write_req;
  write_req->data = endpoint;
  // TODO(murgatroid99): figure out what the return value here means
  uv_write(write_req, (uv_stream_t *)socket->handle, buffers, 1,
           uv_write_callback);
}

static void shutdown_callback(uv_shutdown_t *req, int status) {}

static void uv_endpoint_shutdown(grpc_endpoint *ep) {
  uv_socket* socket = (uv_socket*) grpc_endpoint_get_socket(ep);
  uv_shutdown_t *req = &socket->shutdown_req;
  uv_shutdown(req, (uv_stream_t *)socket->handle, shutdown_callback);
}

static void uv_endpoint_close(grpc_endpoint *ep) {
  uv_socket* socket = (uv_socket*) grpc_endpoint_get_socket(ep);
  uv_close((uv_handle_t *)socket->handle, uv_close_callback);
}

static void* uv_init(grpc_endpoint* endpoint, void* arg) {
  /* Disable Nagle's Algorithm */
  uv_socket* socket = gpr_malloc(sizeof(uv_socket));
  socket->handle = (uv_tcp_t*) arg;
  socket->handle->data = endpoint;
  socket->write_buffers = NULL;
  socket->write_len = 0;
  socket->read_len = 0;
  uv_tcp_nodelay(socket->handle, 1);
#ifndef GRPC_UV_TCP_HOLD_LOOP
  uv_unref((uv_handle_t *)socket->handle);
#endif
 return socket;
}

grpc_socket_vtable uv_socket_vtable = {
  uv_init, uv_destroy, uv_endpoint_shutdown, uv_endpoint_close, uv_endpoint_write, uv_async_read};


#endif

