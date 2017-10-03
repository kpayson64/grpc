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

#define IGNORE_CONST(addr) ((struct sockaddr*)(uintptr_t)(addr))

grpc_tracer_flag grpc_tcp_trace = GRPC_TRACER_INITIALIZER(true, "tcp");

typedef struct {
  uv_connect_t connect_req;
  uv_write_t write_req;
  uv_shutdown_t shutdown_req;
  uv_tcp_t *handle;
  uv_buf_t *write_buffers;
  size_t write_len;

  char* read_buf;
  size_t read_len;

  bool accept_ready;
  bool pending_connections;
} uv_socket;

static void uv_socket_destroy(grpc_socket_wrapper* sw) {
  uv_socket* s = sw->socket;
  gpr_free(s->handle);
  gpr_free(sw);
}

static void alloc_uv_buf(uv_handle_t *handle, size_t suggested_size,
                         uv_buf_t *buf) {
  uv_socket *s = (uv_socket*)((grpc_socket_wrapper*)handle->data)->socket;
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
  grpc_custom_read_callback((grpc_socket_wrapper*)stream->data, (size_t)nread, error);
}

//TODO DO SOMETHING ABOUT THE UNREF
static void uv_close_callback(uv_handle_t *handle) {
  grpc_custom_close_callback((grpc_socket_wrapper*)handle->data);
}

static void uv_socket_read(grpc_socket_wrapper *sw, char* buffer, size_t length) {
  uv_socket *s = (uv_socket*) sw->socket;
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
    grpc_custom_read_callback(sw, 0, error);
  }
}

static void uv_write_callback(uv_write_t *req, int status) {
  grpc_socket_wrapper* s_wrapper = (grpc_socket_wrapper*) req->data;
  uv_socket *s = (uv_socket*) s_wrapper->socket;
  grpc_error *error;
  if (status == 0) {
    error = GRPC_ERROR_NONE;
  } else {
    error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("TCP Write failed");
  }
  gpr_free(s->write_buffers);
  grpc_custom_write_callback(s_wrapper, s->write_len, error);
}

void uv_socket_write(grpc_socket_wrapper* s, char* buffer, size_t length) {
  uv_socket* socket = (uv_socket*) s->socket;
  uv_buf_t *buffers;
  uv_write_t *write_req;

  buffers = gpr_malloc(sizeof(uv_buf_t));
  buffers[0].base = buffer;
  buffers[0].len = length;

  socket->write_buffers = buffers;
  socket->write_len = length;
  write_req = &socket->write_req;
  write_req->data = s;
  // TODO(murgatroid99): figure out what the return value here means
  uv_write(write_req, (uv_stream_t *)socket->handle, buffers, 1,
           uv_write_callback);
}

static void shutdown_callback(uv_shutdown_t *req, int status) {}

static void uv_socket_shutdown(grpc_socket_wrapper *s) {
  uv_socket* socket = (uv_socket*) s->socket;
  uv_shutdown_t *req = &socket->shutdown_req;
  uv_shutdown(req, (uv_stream_t *)socket->handle, shutdown_callback);
}

static void uv_socket_close(grpc_socket_wrapper *s) {
  uv_socket* socket = (uv_socket*) s->socket;
  uv_close((uv_handle_t *)socket->handle, uv_close_callback);
}

static grpc_error* uv_socket_init(grpc_socket_wrapper* s, int domain) {
  /* Disable Nagle's Algorithm */
  uv_socket* socket = gpr_malloc(sizeof(uv_socket));
  uv_tcp_t* tcp = gpr_malloc(sizeof(uv_tcp_t));
  int status = uv_tcp_init_ex(uv_default_loop(), tcp, (unsigned int) domain);
  socket->handle = tcp;
  socket->handle->data = s;
  socket->write_buffers = NULL;
  socket->write_len = 0;
  socket->read_len = 0;
  uv_tcp_nodelay(socket->handle, 1);
#ifndef GRPC_UV_TCP_HOLD_LOOP
  uv_unref((uv_handle_t *)socket->handle);
#endif
 s->socket = socket;
 if (status != 0) {
    grpc_error* error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "Failed to initialize UV tcp handle");
    error =
        grpc_error_set_str(error, GRPC_ERROR_STR_OS_ERROR,
                           grpc_slice_from_static_string(uv_strerror(status)));
   return error;
 }
 return GRPC_ERROR_NONE;
}

static grpc_error* uv_socket_getpeername(grpc_socket_wrapper* socket, const struct sockaddr* addr, int* addr_len) {
  uv_socket* s = (uv_socket*) socket->socket;
  int err = uv_tcp_getpeername(s->handle, IGNORE_CONST(addr), addr_len);
  if (err == 0) {
   return GRPC_ERROR_NONE;
  } else {
    grpc_error* error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("getpeername failed");
    error =
        grpc_error_set_str(error, GRPC_ERROR_STR_OS_ERROR,
                           grpc_slice_from_static_string(uv_strerror(err)));
    return error;
  }
}

static grpc_error* uv_socket_getsockname(grpc_socket_wrapper* socket, const struct sockaddr* addr, int* addr_len) {
  uv_socket* s = (uv_socket*) socket->socket;
  int err = uv_tcp_getsockname(s->handle, IGNORE_CONST(addr), addr_len);
  if (err == 0) {
   return GRPC_ERROR_NONE;
  } else {
    grpc_error* error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("getsockname failed");
    error =
        grpc_error_set_str(error, GRPC_ERROR_STR_OS_ERROR,
                           grpc_slice_from_static_string(uv_strerror(err)));
    return error;
  }
}

static void uv_on_connect(uv_stream_t *server, int status) {
  grpc_socket_wrapper* socket = (grpc_socket_wrapper *)server->data;
  uv_socket* s = (uv_socket*)socket->socket;
  gpr_log(GPR_ERROR, "ON CONNECT CALLED!");
  if (status < 0) {
    switch (status) {
      case UV_EINTR:
      case UV_EAGAIN:
        return;
      default:
        grpc_custom_accept_callback(socket, NULL, GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                                                  "Accept Failed"));
        return;
    }
  }

  if (s->accept_ready) {
    grpc_socket_wrapper* client = gpr_malloc(sizeof(grpc_socket_wrapper));
    client->endpoint = NULL;
    client->listener = NULL;
    uv_socket_init(client, 0);
    // UV documentation says this is guaranteed to succeed
    uv_accept((uv_stream_t *)s->handle, (uv_stream_t *)((uv_socket*)client->socket)->handle);
    gpr_log(GPR_ERROR, "%p PTR",  ((uv_socket*)client->socket)->handle);
    grpc_custom_accept_callback(socket, client, GRPC_ERROR_NONE);
  } else {
    s->pending_connections = true;
  }
}

grpc_error* uv_socket_accept(grpc_socket_wrapper* socket) {
  uv_socket* s = (uv_socket*)socket->socket;
  s->accept_ready = true;
  gpr_log(GPR_ERROR, "HAS PENDING CONNECTIONS");
  if (s->pending_connections) {
    s->pending_connections = false;
    grpc_socket_wrapper* client = gpr_malloc(sizeof(grpc_socket_wrapper));
    client->endpoint = NULL;
    client->listener = NULL;
    uv_socket_init(client, 0);
    uv_accept((uv_stream_t *)s->handle, (uv_stream_t *)((uv_socket*)client->socket)->handle);
    grpc_custom_accept_callback(socket, client, GRPC_ERROR_NONE);
  }
  return GRPC_ERROR_NONE;
}

static grpc_error* uv_socket_bind(grpc_socket_wrapper* socket, const struct sockaddr* addr, int flags) {
  uv_socket* s = (uv_socket*) socket->socket;
  int status = uv_tcp_bind((uv_tcp_t *) s->handle, addr, 0);
  if (status != 0) {
    grpc_error* error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("Failed to bind to port");
    error =
        grpc_error_set_str(error, GRPC_ERROR_STR_OS_ERROR,
                           grpc_slice_from_static_string(uv_strerror(status)));
    return error;
  }
  return GRPC_ERROR_NONE;
}

static grpc_error* uv_socket_listen(grpc_socket_wrapper* socket) {
  uv_socket* s = (uv_socket*) socket->socket;
  int status = uv_listen((uv_stream_t *)s->handle, SOMAXCONN, uv_on_connect);
  if (status != 0) {
    grpc_error* error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("Failed to listen to port");
    error =
        grpc_error_set_str(error, GRPC_ERROR_STR_OS_ERROR,
                           grpc_slice_from_static_string(uv_strerror(status)));
    return error;
  }
  return GRPC_ERROR_NONE;
}

static grpc_error* uv_socket_setsockopt(grpc_socket_wrapper* socket, int level, int option_name, const void* optval, socklen_t option_len) {
  int fd;
  uv_socket* s = (uv_socket*) socket->socket;
  uv_fileno((uv_handle_t *)s->handle, &fd);
  setsockopt(fd, level, option_name, &optval, (socklen_t) option_len);
  return GRPC_ERROR_NONE;
}

static void uv_tc_on_connect(uv_connect_t *req, int status) {
  grpc_socket_wrapper* socket = req->data;
  grpc_error* error;
  if (status == 0) {
    error = GRPC_ERROR_NONE;
  } else {
    error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "Failed to connect to remote host");
    error = grpc_error_set_int(error, GRPC_ERROR_INT_ERRNO, -status);
    error =
        grpc_error_set_str(error, GRPC_ERROR_STR_OS_ERROR,
                           grpc_slice_from_static_string(uv_strerror(status)));
    if (status == UV_ECANCELED) {
      error =
          grpc_error_set_str(error, GRPC_ERROR_STR_OS_ERROR,
                             grpc_slice_from_static_string("Timeout occurred"));
      // This should only happen if the handle is already closed
    } else {
      error = grpc_error_set_str(
          error, GRPC_ERROR_STR_OS_ERROR,
          grpc_slice_from_static_string(uv_strerror(status)));
      uv_socket_close(socket);
    }
  }
  grpc_custom_connect_callback(socket, error);
}

static void uv_socket_connect(grpc_socket_wrapper* s, const struct sockaddr* addr, size_t len) {
  uv_socket* socket = (uv_socket*) s->socket;
  socket->connect_req.data = s;
  // TODO(murgatroid99): figure out what the return value here means
  uv_tcp_connect(&socket->connect_req, socket->handle,
                 addr, uv_tc_on_connect);
}



grpc_socket_vtable uv_socket_vtable = { uv_socket_init, uv_socket_connect, uv_socket_destroy, uv_socket_shutdown, uv_socket_close,
  uv_socket_write, uv_socket_read, uv_socket_getpeername, uv_socket_getsockname, uv_socket_setsockopt,
  uv_socket_bind, uv_socket_listen, uv_socket_accept};


#endif

