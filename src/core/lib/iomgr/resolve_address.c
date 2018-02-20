/*
 *
 * Copyright 2018 gRPC authors.
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

#include <grpc/support/alloc.h>
#include "src/core/lib/iomgr/resolve_address.h"


extern grpc_address_resolver_vtable grpc_default_resolver_vtable;
static grpc_address_resolver_vtable* resolver_vtable = NULL;

void grpc_set_resolver_impl(grpc_address_resolver_vtable* vtable) {
  resolver_vtable = vtable;
}

void grpc_global_resolver_init() {
  if (resolver_vtable == NULL) {
    resolver_vtable = &grpc_default_resolver_vtable;
  }
}

void grpc_resolve_address(grpc_exec_ctx *exec_ctx, const char *addr,
                                    const char *default_port,
                                    grpc_pollset_set *interested_parties,
                                    grpc_closure *on_done,
                                    grpc_resolved_addresses **addresses) {
  resolver_vtable->resolve_address(exec_ctx, addr, default_port, interested_parties, on_done, addresses);
}

void grpc_resolved_addresses_destroy(grpc_resolved_addresses *addrs) {
  if (addrs != NULL) {
    gpr_free(addrs->addrs);
  }
  gpr_free(addrs);
}

grpc_error* grpc_blocking_resolve_address(
    const char *name, const char *default_port,
    grpc_resolved_addresses **addresses) {
  return resolver_vtable->resolve_address_blocking(name, default_port, addresses);
}

