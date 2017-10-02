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

#ifndef GRPC_CORE_LIB_IOMGR_TCP_UV_H
#define GRPC_CORE_LIB_IOMGR_TCP_UV_H
/*
   Low level TCP "bottom half" implementation, for use by transports built on
   top of a TCP connection.

   Note that this file does not (yet) include APIs for creating the socket in
   the first place.

   All calls passing slice transfer ownership of a slice refcount unless
   otherwise specified.
*/

#include <netinet/in.h>

#include "src/core/lib/debug/trace.h"
#include "src/core/lib/iomgr/endpoint.h"

extern grpc_tracer_flag grpc_tcp_trace;

#define GRPC_TCP_DEFAULT_READ_SLICE_SIZE 8192

typedef struct grpc_socket_vtable grpc_socket_vtable;
typedef struct grpc_tcp_listener grpc_tcp_listener;
typedef struct grpc_uv_tcp_connect grpc_uv_tcp_connect;

typedef struct grpc_socket_wrapper {
  // Implementation defined
  void* socket;
  grpc_socket_vtable* vtable;

  // Set if this socket is a connect endpoint, or null otherwise
  grpc_endpoint* endpoint;

  // Set if this socket is listening, or null otherwise
  grpc_tcp_listener* listener;

  // Set if this socket is connecting or null otherwise
  grpc_uv_tcp_connect* connector;
} grpc_socket_wrapper;


typedef struct grpc_socket_vtable {
  void (*init)(grpc_socket_wrapper* s, int family);
  void (*connect)(grpc_socket_wrapper* s, const struct sockaddr* addr, size_t len);
  void (*destroy)(grpc_socket_wrapper* s);
  void (*shutdown)(grpc_socket_wrapper* s);
  void (*close)(grpc_socket_wrapper* s);
  void (*write)(grpc_socket_wrapper* s, char* buffer, size_t length);
  void (*read)(grpc_socket_wrapper* s, char* buffer, size_t length);
  grpc_error* (*getpeername)(grpc_socket_wrapper* s, const struct sockaddr* addr, int* len);
  grpc_error* (*getsockname)(grpc_socket_wrapper* s, const struct sockaddr* addr, int* len);
  grpc_error* (*setsockopt)(grpc_socket_wrapper* s, int level, int optname,
             		    const void *optval, socklen_t optlen);
  grpc_error* (*bind)(grpc_socket_wrapper* s, const struct sockaddr* addr, int flags);
  grpc_error* (*listen)(grpc_socket_wrapper* s);
  grpc_error* (*accept)(grpc_socket_wrapper* s);
} grpc_socket_vtable;

grpc_endpoint *custom_tcp_endpoint_create(grpc_socket_wrapper *socket,
                                          grpc_resource_quota *resource_quota,
                                          char *peer_string);

void grpc_custom_connect_callback(grpc_socket_wrapper* s, grpc_error* error);
void grpc_custom_write_callback(grpc_socket_wrapper* s, size_t nwritten, grpc_error* error);
void grpc_custom_read_callback(grpc_socket_wrapper* s, size_t nread, grpc_error* error);
void grpc_custom_accept_callback(grpc_socket_wrapper* s, grpc_socket_wrapper* new_socket, grpc_error* error);
void grpc_custom_close_callback(grpc_socket_wrapper* s);

void grpc_custom_close_listener_callback(grpc_tcp_listener* listener);

#endif /* GRPC_CORE_LIB_IOMGR_TCP_UV_H */
