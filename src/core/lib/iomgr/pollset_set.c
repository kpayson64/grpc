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

#include "src/core/lib/iomgr/pollset_set.h"


static grpc_pollset_set_vtable* pollset_set_vtable = NULL;

void grpc_set_pollset_set_vtable(grpc_pollset_set_vtable* vtable) {
  pollset_set_vtable = vtable;
}

void grpc_pollset_set_global_init() {
  if (pollset_set_vtable == NULL) {
    pollset_set_vtable = grpc_default_pollset_set_vtable();
  }
}

grpc_pollset_set *grpc_pollset_set_create() {
  return pollset_set_vtable->create();
}

void grpc_pollset_set_destroy(grpc_exec_ctx *exec_ctx,
                              grpc_pollset_set *pollset_set) {
  pollset_set_vtable->destroy(exec_ctx, pollset_set);
}

void grpc_pollset_set_add_pollset(grpc_exec_ctx *exec_ctx,
                                  grpc_pollset_set *pollset_set,
                                  grpc_pollset *pollset) {
  pollset_set_vtable->add_pollset(exec_ctx, pollset_set, pollset);
}

void grpc_pollset_set_del_pollset(grpc_exec_ctx *exec_ctx,
                                  grpc_pollset_set *pollset_set,
                                  grpc_pollset *pollset) {
  pollset_set_vtable->del_pollset(exec_ctx, pollset_set, pollset);
}

void grpc_pollset_set_add_pollset_set(grpc_exec_ctx *exec_ctx,
                                      grpc_pollset_set *bag,
                                      grpc_pollset_set *item) {
  pollset_set_vtable->add_pollset_set(exec_ctx, bag, item);
}

void grpc_pollset_set_del_pollset_set(grpc_exec_ctx *exec_ctx,
                                      grpc_pollset_set *bag,
                                      grpc_pollset_set *item) {
  pollset_set_vtable->del_pollset_set(exec_ctx, bag, item);
}
