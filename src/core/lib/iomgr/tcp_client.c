/*
 *
 * Copyright 2017 gRPC authors.
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

#include "src/core/lib/iomgr/tcp_client.h"

extern grpc_tcp_client_vtable* default_tcp_client_vtable;

static grpc_tcp_client_vtable* client_vtable = NULL;

void grpc_tcp_client_connect(grpc_exec_ctx *exec_ctx, grpc_closure *closure,
                             grpc_endpoint **ep,
                             grpc_pollset_set *interested_parties,
                             const grpc_channel_args *channel_args,
                             const grpc_resolved_address *addr,
                             gpr_timespec deadline) {
  client_vtable->connect(exec_ctx, closure, ep, interested_parties,
                         channel_args, addr, deadline);
}

void grpc_tcp_client_init() {
  if (client_vtable == NULL) {
    client_vtable = default_tcp_client_vtable;
  }
}

void grpc_set_tcp_client_impl(grpc_tcp_client_vtable* impl) {
  client_vtable = impl;
}
