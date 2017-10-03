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

#include <limits.h>
#include <string.h>

#include <grpc/slice_buffer.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>

#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/iomgr_custom.h"
#include "src/core/lib/iomgr/network_status_tracker.h"
#include "src/core/lib/iomgr/resource_quota.h"
#include "src/core/lib/iomgr/tcp_custom.h"
#include "src/core/lib/iomgr/tcp_client.h"
#include "src/core/lib/iomgr/tcp_server.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"

#define GRPC_TRACER_ON_TCP_TRACE 0

grpc_socket_vtable* grpc_custom_socket_vtable = NULL;
extern grpc_tcp_server_vtable custom_tcp_server_vtable;
extern grpc_tcp_client_vtable custom_tcp_client_vtable;

void grpc_custom_endpoint_init(grpc_socket_vtable* impl) {
  grpc_custom_socket_vtable = impl;
  grpc_set_tcp_client_impl(&custom_tcp_client_vtable);
  grpc_set_tcp_server_impl(&custom_tcp_server_vtable);
}

typedef struct {
  grpc_endpoint base;
  gpr_refcount refcount;
  grpc_socket_wrapper* socket;

  grpc_closure *read_cb;
  grpc_closure *write_cb;

  grpc_slice read_slice;
  grpc_slice_buffer *read_slices;
  grpc_slice_buffer *write_slices;
  size_t outgoing_slice_idx;
  size_t outgoing_byte_idx;

  grpc_resource_user *resource_user;

  bool shutting_down;

  char *peer_string;
  grpc_pollset *pollset;
} custom_tcp_endpoint;

static void tcp_free(grpc_exec_ctx *exec_ctx, grpc_socket_wrapper *s) {
  custom_tcp_endpoint* tcp = (custom_tcp_endpoint*) s;
  grpc_custom_socket_vtable->destroy(s);
  grpc_slice_unref_internal(exec_ctx, tcp->read_slice);
  grpc_resource_user_unref(exec_ctx, tcp->resource_user);
  gpr_free(tcp->peer_string);
  gpr_free(tcp);
}

#ifndef NDEBUG
#define TCP_UNREF(exec_ctx, tcp, reason) \
  tcp_unref((exec_ctx), (tcp), (reason), __FILE__, __LINE__)
#define TCP_REF(tcp, reason) tcp_ref((tcp), (reason), __FILE__, __LINE__)
static void tcp_unref(grpc_exec_ctx *exec_ctx, custom_tcp_endpoint *tcp,
                      const char *reason, const char *file, int line) {
  if (GRPC_TRACER_ON_TCP_TRACE) {
    gpr_atm val = gpr_atm_no_barrier_load(&tcp->refcount.count);
    gpr_log(file, line, GPR_LOG_SEVERITY_DEBUG,
            "TCP unref %p : %s %" PRIdPTR " -> %" PRIdPTR, tcp, reason, val,
            val - 1);
  }
  if (gpr_unref(&tcp->refcount)) {
    tcp_free(exec_ctx, tcp->socket);
  }
}

static void tcp_ref(custom_tcp_endpoint *tcp, const char *reason, const char *file,
                    int line) {
  if (GRPC_TRACER_ON_TCP_TRACE) {
    gpr_atm val = gpr_atm_no_barrier_load(&tcp->refcount.count);
    gpr_log(file, line, GPR_LOG_SEVERITY_DEBUG,
            "TCP   ref %p : %s %" PRIdPTR " -> %" PRIdPTR, tcp, reason, val,
            val + 1);
  }
  gpr_ref(&tcp->refcount);
}
#else
#define TCP_UNREF(exec_ctx, tcp, reason) tcp_unref((exec_ctx), (tcp))
#define TCP_REF(tcp, reason) tcp_ref((tcp))
static void tcp_unref(grpc_exec_ctx *exec_ctx, custom_tcp_endpoint *tcp) {
  if (gpr_unref(&tcp->refcount)) {
    tcp_free(exec_ctx, tcp->socket);
  }
}

static void tcp_ref(custom_tcp_endpoint *tcp) { gpr_ref(&tcp->refcount); }
#endif

static grpc_slice alloc_read_slice(grpc_exec_ctx *exec_ctx,
                                   grpc_resource_user *resource_user) {
  return grpc_resource_user_slice_malloc(exec_ctx, resource_user,
                                         GRPC_TCP_DEFAULT_READ_SLICE_SIZE);
}

void grpc_custom_read_callback(grpc_socket_wrapper* s, size_t nread, grpc_error* error) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  custom_tcp_endpoint *tcp = (custom_tcp_endpoint*) s->endpoint;
  grpc_closure *cb = tcp->read_cb;
  tcp->read_cb = NULL;
  grpc_slice sub;
  if (error == GRPC_ERROR_NONE) {
    // Successful read
    sub = grpc_slice_sub_no_ref(tcp->read_slice, 0, (size_t)nread);
    grpc_slice_buffer_add(tcp->read_slices, sub);
    tcp->read_slice = alloc_read_slice(&exec_ctx, tcp->resource_user);
    if (GRPC_TRACER_ON_TCP_TRACE) {
      size_t i;
      for (i = 0; i < tcp->read_slices->count; i++) {
        char *dump = grpc_dump_slice(tcp->read_slices->slices[i],
                                     GPR_DUMP_HEX | GPR_DUMP_ASCII);
        gpr_log(GPR_DEBUG, "READ %p (peer=%s): %s", tcp, tcp->peer_string,
                dump);
        gpr_free(dump);
      }
    }
  } else {
    if (GRPC_TRACER_ON_TCP_TRACE) {
      const char *str = grpc_error_string(error);
      gpr_log(GPR_DEBUG, "read: error=%s", str);
    }
  }
  TCP_UNREF(&exec_ctx, tcp, "read");
  GRPC_CLOSURE_SCHED(&exec_ctx, cb, error);
  grpc_exec_ctx_finish(&exec_ctx);
}


static void custom_endpoint_read(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
                                 grpc_slice_buffer *read_slices, grpc_closure *cb) {
  custom_tcp_endpoint *tcp = (custom_tcp_endpoint *)ep;
  grpc_error *error = GRPC_ERROR_NONE;
  GRPC_UV_ASSERT_SAME_THREAD();
  GPR_ASSERT(tcp->read_cb == NULL);
  tcp->read_cb = cb;
  tcp->read_slices = read_slices;
  grpc_slice_buffer_reset_and_unref_internal(exec_ctx, read_slices);
  TCP_REF(tcp, "read");

  if (GRPC_TRACER_ON_TCP_TRACE) {
    const char *str = grpc_error_string(error);
    gpr_log(GPR_DEBUG, "Initiating read on %p: error=%s", tcp, str);
  }

  char* buffer = (char*) GRPC_SLICE_START_PTR(tcp->read_slice);
  size_t len = GRPC_SLICE_LENGTH(tcp->read_slice);
  grpc_custom_socket_vtable->read(tcp->socket, buffer, len);
}

void grpc_custom_write_callback(grpc_socket_wrapper* socket, size_t nwritten, grpc_error* error) {
  custom_tcp_endpoint *tcp = (custom_tcp_endpoint*) socket->endpoint;
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_closure *cb = tcp->write_cb;
  tcp->write_cb = NULL;
  if (error == GRPC_ERROR_NONE) {
    tcp->outgoing_byte_idx += nwritten;
    if (tcp->outgoing_byte_idx == GRPC_SLICE_LENGTH(tcp->write_slices->slices[tcp->outgoing_slice_idx])) {
      tcp->outgoing_slice_idx++;
      tcp->outgoing_byte_idx = 0;
    }
    if (tcp->outgoing_slice_idx == tcp->write_slices->count) {
      if (GRPC_TRACER_ON_TCP_TRACE) {
        const char *str = grpc_error_string(error);
        gpr_log(GPR_DEBUG, "write complete on %p: error=%s", tcp, str);
      }
      TCP_UNREF(&exec_ctx, tcp, "write");
      GRPC_CLOSURE_SCHED(&exec_ctx, cb, error);
    } else {
       char* buffer = (char*) GRPC_SLICE_START_PTR(tcp->write_slices->slices[tcp->outgoing_slice_idx]);
       buffer += tcp->outgoing_byte_idx;
       size_t len = GRPC_SLICE_LENGTH(tcp->write_slices->slices[tcp->outgoing_slice_idx]);
       len -= tcp->outgoing_byte_idx;
       tcp->write_cb = cb;
       grpc_custom_socket_vtable->write(socket, buffer, len);
    }
  } else {
    if (GRPC_TRACER_ON_TCP_TRACE) {
      const char *str = grpc_error_string(error);
      gpr_log(GPR_DEBUG, "write complete on %p: error=%s", tcp, str);
    }
    TCP_UNREF(&exec_ctx, tcp, "write");
    GRPC_CLOSURE_SCHED(&exec_ctx, cb, error);
  }
  grpc_exec_ctx_finish(&exec_ctx);
}


static void custom_endpoint_write(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
                              grpc_slice_buffer *write_slices,
                              grpc_closure *cb) {
  custom_tcp_endpoint *tcp = (custom_tcp_endpoint *)ep;
  GRPC_UV_ASSERT_SAME_THREAD();

  if (GRPC_TRACER_ON_TCP_TRACE) {
    size_t j;

    for (j = 0; j < write_slices->count; j++) {
      char *data = grpc_dump_slice(write_slices->slices[j],
                                   GPR_DUMP_HEX | GPR_DUMP_ASCII);
      gpr_log(GPR_DEBUG, "WRITE %p (peer=%s): %s", tcp, tcp->peer_string, data);
      gpr_free(data);
    }
  }

  if (tcp->shutting_down) {
    GRPC_CLOSURE_SCHED(exec_ctx, cb, GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                                         "TCP socket is shutting down"));
    return;
  }

  GPR_ASSERT(tcp->write_cb == NULL);
  tcp->write_slices = write_slices;
  GPR_ASSERT(tcp->write_slices->count <= UINT_MAX);
  if (tcp->write_slices->count == 0) {
    // No slices means we don't have to do anything,
    // and libuv doesn't like empty writes
    GRPC_CLOSURE_SCHED(exec_ctx, cb, GRPC_ERROR_NONE);
    return;
  }
  tcp->outgoing_slice_idx=0;
  tcp->outgoing_byte_idx=0;
  char* buffer = (char*) GRPC_SLICE_START_PTR(tcp->write_slices->slices[tcp->outgoing_slice_idx]);
  size_t len = GRPC_SLICE_LENGTH(tcp->write_slices->slices[tcp->outgoing_slice_idx]);

  tcp->write_cb = cb;
  TCP_REF(tcp, "write");
  grpc_custom_socket_vtable->write(tcp->socket, buffer, len);
}

static void custom_add_to_pollset(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
                              grpc_pollset *pollset) {
  // No-op. We're ignoring pollsets currently
  (void)exec_ctx;
  (void)ep;
  (void)pollset;
  custom_tcp_endpoint *tcp = (custom_tcp_endpoint *)ep;
  tcp->pollset = pollset;
}

static void custom_add_to_pollset_set(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
                                  grpc_pollset_set *pollset) {
  // No-op. We're ignoring pollsets currently
  (void)exec_ctx;
  (void)ep;
  (void)pollset;
}

static void custom_endpoint_shutdown(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
                                 grpc_error *why) {
  custom_tcp_endpoint *tcp = (custom_tcp_endpoint *)ep;
  if (!tcp->shutting_down) {
    if (GRPC_TRACER_ON_TCP_TRACE) {
      const char *str = grpc_error_string(why);
      gpr_log(GPR_DEBUG, "TCP %p shutdown why=%s", tcp->socket, str);
    }
    tcp->shutting_down = true;
    grpc_resource_user_shutdown(exec_ctx, tcp->resource_user);
    grpc_custom_socket_vtable->shutdown(tcp->socket);
  }
  GRPC_ERROR_UNREF(why);
}

static void custom_destroy(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep) {
  grpc_network_status_unregister_endpoint(ep);
  custom_tcp_endpoint *tcp = (custom_tcp_endpoint *)ep;
  grpc_custom_socket_vtable->close(tcp->socket);
}

static char *custom_get_peer(grpc_endpoint *ep) {
  custom_tcp_endpoint *tcp = (custom_tcp_endpoint *)ep;
  return gpr_strdup(tcp->peer_string);
}

static grpc_resource_user *custom_get_resource_user(grpc_endpoint *ep) {
  custom_tcp_endpoint *tcp = (custom_tcp_endpoint *)ep;
  return tcp->resource_user;
}

static int custom_get_fd(grpc_endpoint *ep) { return -1; }

static grpc_endpoint_vtable vtable = {
    custom_endpoint_read,      custom_endpoint_write,    custom_add_to_pollset,
    custom_add_to_pollset_set, custom_endpoint_shutdown, custom_destroy,
    custom_get_resource_user,  custom_get_peer,          custom_get_fd};

grpc_endpoint *custom_tcp_endpoint_create(grpc_socket_wrapper* socket,
					  grpc_resource_quota *resource_quota,
                                          char *peer_string) {
  custom_tcp_endpoint *tcp = (custom_tcp_endpoint *)gpr_malloc(sizeof(custom_tcp_endpoint));
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;

  if (GRPC_TRACER_ON_TCP_TRACE) {
    gpr_log(GPR_DEBUG, "Creating TCP endpoint %p", tcp);
  }
  memset(tcp, 0, sizeof(custom_tcp_endpoint));
  socket->endpoint = (grpc_endpoint*) tcp;
  tcp->socket = socket;
  tcp->base.vtable = &vtable;
  gpr_ref_init(&tcp->refcount, 1);
  tcp->peer_string = gpr_strdup(peer_string);
  tcp->shutting_down = false;
  tcp->resource_user = grpc_resource_user_create(resource_quota,
                                                 peer_string);
  tcp->read_slice = alloc_read_slice(&exec_ctx, tcp->resource_user);
  /* Tell network status tracking code about the new endpoint */
  grpc_network_status_register_endpoint(&tcp->base);

  grpc_exec_ctx_finish(&exec_ctx);
  return &tcp->base;
}
