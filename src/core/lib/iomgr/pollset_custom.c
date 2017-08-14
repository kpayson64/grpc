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

#include <grpc/support/port_platform.h>
#include <grpc/support/sync.h>

#include "src/core/lib/iomgr/iomgr_custom.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/iomgr/pollset.h"

static gpr_mu g_polling_mu;

struct grpc_pollset {
  int dummy_struct;
};

static void pollset_global_init() {
  gpr_mu_init(&g_polling_mu);
}

static void pollset_global_shutdown() {
  gpr_mu_destroy(&g_polling_mu);
}

static void pollset_init(grpc_pollset *pollset, gpr_mu **mu) {
  *mu = &g_polling_mu;
}

/* Begin shutting down the pollset, and call closure when done.
 * pollset's mutex must be held */
static void pollset_shutdown(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset,
                           grpc_closure *closure) {
  GRPC_UV_ASSERT_SAME_THREAD();
  GRPC_CLOSURE_SCHED(exec_ctx, closure, GRPC_ERROR_NONE);
}

static void pollset_destroy(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset) {
  GRPC_UV_ASSERT_SAME_THREAD();
}

static grpc_error *pollset_work(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset,
                              grpc_pollset_worker **worker, gpr_timespec now,
                              gpr_timespec deadline) {
  GRPC_UV_ASSERT_SAME_THREAD();
  return GRPC_ERROR_NONE;
}

static grpc_error *pollset_kick(grpc_pollset *pollset,
                              grpc_pollset_worker *specific_worker) {
  GRPC_UV_ASSERT_SAME_THREAD();
  return GRPC_ERROR_NONE;
}

grpc_pollset_vtable custom_pollset_vtable = {
  pollset_global_init, pollset_global_shutdown, pollset_init,
  pollset_shutdown, pollset_destroy, pollset_work, pollset_kick};
