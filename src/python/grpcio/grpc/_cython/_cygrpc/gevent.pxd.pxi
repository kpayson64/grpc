# Copyright 2017 gRPC authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


cdef extern from "src/core/lib/iomgr/error.h":
  struct grpc_error:
    pass

cdef extern from "sys/socket.h":
  struct sockaddr:
    pass
  ctypedef unsigned int socklen_t
  

cdef extern from "netdb.h":
    struct addrinfo:
        int ai_flags       # input flags
        int ai_family      # protocol family for socket
        int ai_socktype    # socket type
        int ai_protocol    # protocol for socket
        int ai_addrlen     # length of socket-address
        sockaddr *ai_addr  # socket-address for socket
        char *ai_canonname # canonical name for service location
        addrinfo *ai_next  # pointer to next in list

cdef extern from "src/core/lib/iomgr/tcp_custom.h":
  struct grpc_socket_wrapper:
    void* socket
    # We don't care about the rest of the fields
  struct grpc_resolve_wrapper:
    pass

  struct grpc_socket_vtable:
    void (*resolve)(grpc_resolve_wrapper* r, char* host, char* port, addrinfo* hints, int blocking)
    grpc_error* (*init)(grpc_socket_wrapper* s, void* socket, int domain)
    void (*connect)(grpc_socket_wrapper* s, const sockaddr* addr, size_t len)
    void (*destroy)(grpc_socket_wrapper* s)
    void (*shutdown)(grpc_socket_wrapper* s)
    void (*close)(grpc_socket_wrapper* s)
    void (*write)(grpc_socket_wrapper* s, char* buffer, size_t length)
    void (*read)(grpc_socket_wrapper* s, char* buffer, size_t length)
    grpc_error* (*getpeername)(grpc_socket_wrapper* s, const sockaddr* addr, int* len)
    grpc_error* (*getsockname)(grpc_socket_wrapper* s, const sockaddr* addr, int* len)
    grpc_error* (*setsockopt)(grpc_socket_wrapper* s, int level, int optname,
                              const void *optval, socklen_t optlen)
    grpc_error* (*bind)(grpc_socket_wrapper* s, sockaddr* addr, size_t len, int flags);
    grpc_error* (*listen)(grpc_socket_wrapper* s)
    grpc_error* (*accept)(grpc_socket_wrapper* s)

  void grpc_custom_resolve_callback(grpc_resolve_wrapper* r, addrinfo* result, grpc_error* error);
  void grpc_custom_connect_callback(grpc_socket_wrapper* s, grpc_error* error);
  void grpc_custom_write_callback(grpc_socket_wrapper* s, size_t nwritten, grpc_error* error);
  void grpc_custom_read_callback(grpc_socket_wrapper* s, size_t nread, grpc_error* error);
  void grpc_custom_accept_callback(grpc_socket_wrapper* s, void* new_socket, grpc_error* error);
  void grpc_custom_close_callback(grpc_socket_wrapper* s);

  void grpc_custom_endpoint_init(grpc_socket_vtable* impl);
  void grpc_custom_resolver_init(grpc_socket_vtable* impl);

cdef extern from "src/core/lib/iomgr/timer_custom.h":
  struct grpc_timer_wrapper:
    void* timer
    int timeout_ms
     # We don't care about the rest of the fields

  struct grpc_custom_timer_vtable:
    grpc_error* (*start)(grpc_timer_wrapper* t);
    grpc_error* (*stop)(grpc_timer_wrapper* t);

  void grpc_custom_timer_init(grpc_custom_timer_vtable* impl);
  void grpc_custom_timer_callback(grpc_timer_wrapper* t, grpc_error* error);

cdef extern from "src/core/lib/iomgr/pollset_custom.h":
  struct grpc_custom_poller_vtable:
    void (*run_loop)(int blocking)
  
  void grpc_custom_pollset_init(grpc_custom_poller_vtable* vtable)

cdef extern from "src/core/lib/iomgr/iomgr_custom.h":
  void grpc_custom_iomgr_init();

cdef extern from "src/core/lib/iomgr/pollset_set.h":
  void grpc_custom_pollset_set_init();

cdef extern from "src/core/lib/iomgr/resolve_address.h":
  ctypedef struct grpc_resolved_address:
    char[128] addr
    size_t len
  
cdef extern from "src/core/lib/iomgr/sockaddr_utils.h":
  int grpc_sockaddr_get_port(const grpc_resolved_address *addr);
  int grpc_sockaddr_to_string(char **out, const grpc_resolved_address *addr,
                              int normalize);
  void grpc_string_to_sockaddr(grpc_resolved_address *out, char* addr, int port);
  int grpc_sockaddr_set_port(const grpc_resolved_address *resolved_addr,
                             int port)


cdef class TimerWrapper:

  cdef grpc_timer_wrapper *c_timer
  cdef object timer
  cdef object event

cdef class SocketWrapper:
  cdef object watcher
  cdef object event
  cdef grpc_socket_wrapper *c_socket
  cdef char* c_buffer
  cdef size_t len

cdef class ResolveWrapper:
  cdef grpc_resolve_wrapper *c_resolver
  cdef char* c_host
  cdef char* c_port
  cdef addrinfo* c_hints
