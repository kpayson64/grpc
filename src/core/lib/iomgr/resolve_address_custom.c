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

#include <grpc/support/alloc.h>
#include <grpc/support/host_port.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/useful.h>

#include "src/core/lib/iomgr/closure.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/iomgr/tcp_custom.h"
#include "src/core/lib/iomgr/iomgr_custom.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/iomgr/sockaddr.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"

#include <string.h>

typedef struct grpc_resolve_wrapper {
  grpc_closure *on_done;
  grpc_resolved_addresses **addresses;
  struct addrinfo *hints;
  char *host;
  char *port;
} grpc_resolve_wrapper;


static grpc_socket_vtable* grpc_custom_socket_vtable = NULL;

static int retry_named_port_failure(grpc_resolve_wrapper *r) {
    // This loop is copied from resolve_address_posix.c
    int blocking = r->on_done == NULL ? 1 : 0;
    char *svc[][2] = {{"http", "80"}, {"https", "443"}};
    for (size_t i = 0; i < GPR_ARRAY_SIZE(svc); i++) {
      if (strcmp(r->port, svc[i][0]) == 0) {
        r->port = gpr_strdup(svc[i][1]);
        grpc_custom_socket_vtable->resolve(r, r->host, r->port, r->hints, blocking);
        return 1;
      }
    }
  return 0;
}

static grpc_error *handle_addrinfo_result(struct addrinfo *result,
                                          grpc_resolved_addresses **addresses) {
  struct addrinfo *resp;
  size_t i;
  (*addresses) = gpr_malloc(sizeof(grpc_resolved_addresses));
  (*addresses)->naddrs = 0;
  for (resp = result; resp != NULL; resp = resp->ai_next) {
    (*addresses)->naddrs++;
  }
  (*addresses)->addrs =
      gpr_malloc(sizeof(grpc_resolved_address) * (*addresses)->naddrs);
  i = 0;
  for (resp = result; resp != NULL; resp = resp->ai_next) {
    gpr_log(GPR_ERROR, "ADDRESS 1");
    memcpy(&(*addresses)->addrs[i].addr, resp->ai_addr, resp->ai_addrlen);
    (*addresses)->addrs[i].len = resp->ai_addrlen;
    i++;
  }

  {
    for (i = 0; i < (*addresses)->naddrs; i++) {
      char *buf;
      grpc_sockaddr_to_string(&buf, &(*addresses)->addrs[i], 0);
      gpr_free(buf);
    }
  }
  return GRPC_ERROR_NONE;
}

void grpc_custom_resolve_callback(grpc_resolve_wrapper* r, struct addrinfo* result, grpc_error* error) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  if (error == GRPC_ERROR_NONE) {
    handle_addrinfo_result(result, r->addresses);
  } else if (retry_named_port_failure(r)) {
    return;
  }

  if (r->on_done) {
    GRPC_CLOSURE_SCHED(&exec_ctx, r->on_done, error);
  }
  grpc_exec_ctx_finish(&exec_ctx);
  gpr_free(r->hints);
  gpr_free(r->host);
  gpr_free(r->port);
  gpr_free(r);
}

static grpc_error *try_split_host_port(const char *name,
                                       const char *default_port, char **host,
                                       char **port) {
  /* parse name, splitting it into host and port parts */
  grpc_error *error;
  gpr_split_host_port(name, host, port);
  if (*host == NULL) {
    char *msg;
    gpr_asprintf(&msg, "unparseable host:port: '%s'", name);
    error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(msg);
    gpr_free(msg);
    return error;
  }
  if (*port == NULL) {
    // TODO(murgatroid99): add tests for this case
    if (default_port == NULL) {
      char *msg;
      gpr_asprintf(&msg, "no port in name '%s'", name);
      error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(msg);
      gpr_free(msg);
      return error;
    }
    *port = gpr_strdup(default_port);
  }
  return GRPC_ERROR_NONE;
}

static grpc_error *blocking_resolve_address_impl(
    const char *name, const char *default_port,
    grpc_resolved_addresses **addresses) {
  char *host;
  char *port;
  struct addrinfo* hints = gpr_malloc(sizeof(struct addrinfo));
  grpc_error *err;

  GRPC_UV_ASSERT_SAME_THREAD();

  err = try_split_host_port(name, default_port, &host, &port);
  if (err != GRPC_ERROR_NONE) {
    gpr_free(hints);
    gpr_free(host);
    gpr_free(port);
    return err;
  }

  /* Call getaddrinfo */
  memset(hints, 0, sizeof(struct addrinfo));
  hints->ai_family = AF_UNSPEC;     /* ipv4 or ipv6 */
  hints->ai_socktype = SOCK_STREAM; /* stream socket */
  hints->ai_flags = AI_PASSIVE;     /* for wildcard IP address */

  grpc_resolve_wrapper* r = gpr_malloc(sizeof(grpc_resolve_wrapper));
  r->addresses = addresses;
  r->hints = hints;
  r->host = host;
  r->port = port;
  r->on_done = NULL;
  grpc_custom_socket_vtable->resolve(r, r->host, r->port, r->hints, 1);
  // TODO (some how fish out out the return value);
  return GRPC_ERROR_NONE;
}

static void resolve_address_impl(grpc_exec_ctx *exec_ctx, const char *name,
                                 const char *default_port,
                                 grpc_pollset_set *interested_parties,
                                 grpc_closure *on_done,
                                 grpc_resolved_addresses **addrs) {
  grpc_resolve_wrapper *r = NULL;
  struct addrinfo *hints = NULL;
  char *host = NULL;
  char *port = NULL;
  grpc_error *err;
  GRPC_UV_ASSERT_SAME_THREAD();
  err = try_split_host_port(name, default_port, &host, &port);
  if (err != GRPC_ERROR_NONE) {
    GRPC_CLOSURE_SCHED(exec_ctx, on_done, err);
    gpr_free(host);
    gpr_free(port);
    return;
  }
  r = gpr_malloc(sizeof(grpc_resolve_wrapper));
  r->on_done = on_done;
  r->addresses = addrs;
  r->host = host;
  r->port = port;

  /* Call getaddrinfo */
  hints = gpr_malloc(sizeof(struct addrinfo));
  memset(hints, 0, sizeof(struct addrinfo));
  hints->ai_family = AF_UNSPEC;     /* ipv4 or ipv6 */
  hints->ai_socktype = SOCK_STREAM; /* stream socket */
  hints->ai_flags = AI_PASSIVE;     /* for wildcard IP address */
  r->hints = hints;

  grpc_custom_socket_vtable->resolve(r, r->host, r->port, r->hints, 0);
}

grpc_address_resolver_vtable grpc_custom_resolver_vtable = {
  resolve_address_impl, blocking_resolve_address_impl};

void grpc_custom_resolver_init(grpc_socket_vtable* impl) {
  grpc_custom_socket_vtable = impl;
  grpc_set_resolver_impl(&grpc_custom_resolver_vtable);
}

