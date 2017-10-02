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

#include "src/core/lib/iomgr/tcp_server.h"


static grpc_tcp_server_vtable* server_vtable = NULL;

grpc_error *grpc_tcp_server_create(grpc_exec_ctx *exec_ctx,
                                   grpc_closure *shutdown_complete,
                                   const grpc_channel_args *args,
                                   grpc_tcp_server **server) {
  return server_vtable->create(exec_ctx, shutdown_complete, args, server);
}

void grpc_tcp_server_start(grpc_exec_ctx *exec_ctx, grpc_tcp_server *server,
                           grpc_pollset **pollsets, size_t pollset_count,
                           grpc_tcp_server_cb on_accept_cb, void *cb_arg) {
  server_vtable->start(exec_ctx, server, pollsets, pollset_count, on_accept_cb, cb_arg);
}

grpc_error *grpc_tcp_server_add_port(grpc_tcp_server *s,
                                     const grpc_resolved_address *addr,
                                     int *out_port) {
  return server_vtable->add_port(s, addr, out_port);
}

unsigned grpc_tcp_server_port_fd_count(grpc_tcp_server *s, unsigned port_index) {
  return server_vtable->port_fd_count(s, port_index);
}

int grpc_tcp_server_port_fd(grpc_tcp_server *s, unsigned port_index,
                            unsigned fd_index) {
  return server_vtable->port_fd(s, port_index, fd_index);
}

grpc_tcp_server *grpc_tcp_server_ref(grpc_tcp_server *s) {
  return server_vtable->ref(s);
}

void grpc_tcp_server_shutdown_starting_add(grpc_tcp_server *s,
                                           grpc_closure *shutdown_starting) {
  server_vtable->shutdown_starting_add(s, shutdown_starting);
}

void grpc_tcp_server_unref(grpc_exec_ctx *exec_ctx, grpc_tcp_server *s) {
  server_vtable->unref(exec_ctx, s);
}

void grpc_tcp_server_shutdown_listeners(grpc_exec_ctx *exec_ctx,
                                        grpc_tcp_server *s) {
  server_vtable->shutdown_listeners(exec_ctx, s);
}
