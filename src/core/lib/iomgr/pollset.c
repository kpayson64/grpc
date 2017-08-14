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

#include "src/core/lib/iomgr/pollset.h"


static grpc_pollset_vtable* pollset_vtable = NULL;

void grpc_pollset_global_init(void) {
  if (pollset_vtable == NULL) {
    pollset_vtable = grpc_default_pollset_vtable();
  }
  pollset_vtable->global_init();
}

void grpc_pollset_global_shutdown(void) {
  pollset_vtable->global_shutdown();
}

void grpc_pollset_init(grpc_pollset *pollset, gpr_mu **mu) {
  pollset_vtable->init(pollset, mu);
}

void grpc_pollset_shutdown(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset,
                           grpc_closure *closure) {
  pollset_vtable->shutdown(exec_ctx, pollset, closure);
}

void grpc_pollset_destroy(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset) {
  pollset_vtable->destroy(exec_ctx, pollset);
}

grpc_error *grpc_pollset_work(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset,
                              grpc_pollset_worker **worker, gpr_timespec now,
                              gpr_timespec deadline) {
  return pollset_vtable->work(exec_ctx, pollset, worker, now, deadline);
}

grpc_error *grpc_pollset_kick(grpc_pollset *pollset,
                              grpc_pollset_worker *specific_worker) {
  return pollset_vtable->kick(pollset, specific_worker);
}

