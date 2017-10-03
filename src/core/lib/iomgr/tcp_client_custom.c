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

#include <string.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/iomgr_custom.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "src/core/lib/iomgr/tcp_client.h"
#include "src/core/lib/iomgr/tcp_custom.h"
#include "src/core/lib/iomgr/timer.h"

extern grpc_tracer_flag grpc_tcp_trace;
extern grpc_socket_vtable* grpc_custom_socket_vtable;

typedef struct grpc_uv_tcp_connect {
  grpc_socket_wrapper* socket;
  grpc_timer alarm;
  grpc_closure on_alarm;
  grpc_closure *closure;
  grpc_endpoint **endpoint;
  int refs;
  char *addr_name;
  grpc_resource_quota *resource_quota;
} grpc_uv_tcp_connect;

static void uv_tcp_connect_cleanup(grpc_exec_ctx *exec_ctx,
                                   grpc_uv_tcp_connect *connect) {
  grpc_resource_quota_unref_internal(exec_ctx, connect->resource_quota);
  gpr_free(connect->addr_name);
  gpr_free(connect);
}

static void uv_tc_on_alarm(grpc_exec_ctx *exec_ctx, void *acp,
                           grpc_error *error) {
  int done;
  grpc_socket_wrapper* socket = acp;
  grpc_uv_tcp_connect *connect = socket->connector;
  if (GRPC_TRACER_ON(grpc_tcp_trace)) {
    const char *str = grpc_error_string(error);
    gpr_log(GPR_DEBUG, "CLIENT_CONNECT: %s: on_alarm: error=%s",
            connect->addr_name, str);
  }
  if (error == GRPC_ERROR_NONE) {
    /* error == NONE implies that the timer ran out, and wasn't cancelled. If
       it was cancelled, then the handler that cancelled it also should close
       the handle, if applicable */
    grpc_custom_socket_vtable->close(socket);
  }
  done = (--connect->refs == 0);
  if (done) {
    uv_tcp_connect_cleanup(exec_ctx, connect);
  }
}

void grpc_custom_connect_callback(grpc_socket_wrapper* socket, grpc_error* error) {
  grpc_uv_tcp_connect *connect = socket->connector;
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  int done;
  grpc_closure *closure = connect->closure;
  grpc_timer_cancel(&exec_ctx, &connect->alarm);
  if (error == GRPC_ERROR_NONE) {
    gpr_log(GPR_ERROR, "CUSTOM CONNECT CALLBACK");
    *connect->endpoint = custom_tcp_endpoint_create(socket, connect->resource_quota, connect->addr_name);
  }
  done = (--connect->refs == 0);
  if (done) {
    grpc_exec_ctx_flush(&exec_ctx);
    uv_tcp_connect_cleanup(&exec_ctx, connect);
  }
  GRPC_CLOSURE_SCHED(&exec_ctx, closure, error);
  grpc_exec_ctx_finish(&exec_ctx);
}

static void tcp_connect(grpc_exec_ctx *exec_ctx,
                                    grpc_closure *closure, grpc_endpoint **ep,
                                    grpc_pollset_set *interested_parties,
                                    const grpc_channel_args *channel_args,
                                    const grpc_resolved_address *resolved_addr,
                                    gpr_timespec deadline) {
  grpc_uv_tcp_connect *connect;
  grpc_resource_quota *resource_quota = grpc_resource_quota_create(NULL);
  (void)channel_args;
  (void)interested_parties;

  GRPC_UV_ASSERT_SAME_THREAD();

  if (channel_args != NULL) {
    for (size_t i = 0; i < channel_args->num_args; i++) {
      if (0 == strcmp(channel_args->args[i].key, GRPC_ARG_RESOURCE_QUOTA)) {
        grpc_resource_quota_unref_internal(exec_ctx, resource_quota);
        resource_quota = grpc_resource_quota_ref_internal(
            channel_args->args[i].value.pointer.p);
      }
    }
  }

  grpc_socket_wrapper* socket = gpr_malloc(sizeof(grpc_socket_wrapper));
  grpc_custom_socket_vtable->init(socket, 0);
  connect = gpr_zalloc(sizeof(grpc_uv_tcp_connect));
  connect->closure = closure;
  connect->endpoint = ep;
  connect->addr_name = grpc_sockaddr_to_uri(resolved_addr);
  connect->resource_quota = resource_quota;
  socket->connector = connect;

  connect->refs = 1;

  if (GRPC_TRACER_ON(grpc_tcp_trace)) {
    gpr_log(GPR_DEBUG, "CLIENT_CONNECT: %s: asynchronously connecting",
            connect->addr_name);
  }
  grpc_custom_socket_vtable->connect(socket, (const struct sockaddr *)resolved_addr->addr, resolved_addr->len);
  GRPC_CLOSURE_INIT(&connect->on_alarm, uv_tc_on_alarm, socket,
                    grpc_schedule_on_exec_ctx);
  grpc_timer_init(exec_ctx, &connect->alarm,
                  gpr_convert_clock_type(deadline, GPR_CLOCK_MONOTONIC),
                  &connect->on_alarm, gpr_now(GPR_CLOCK_MONOTONIC));
}

grpc_tcp_client_vtable custom_tcp_client_vtable = {tcp_connect};

#ifdef GRPC_UV_TEST
grpc_tcp_client_vtable* default_tcp_client_vtable = &custom_tcp_client_vtable;
#endif

