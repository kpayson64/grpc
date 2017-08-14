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

#include <grpc/support/thd.h>

#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/iomgr/executor.h"
#include "src/core/lib/iomgr/iomgr_internal.h"

gpr_thd_id g_init_thread;

static void iomgr_platform_init(void) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_executor_set_threading(&exec_ctx, false);
  g_init_thread = gpr_thd_currentid();
  grpc_exec_ctx_finish(&exec_ctx);
}
static void iomgr_platform_flush(void) {}
static void iomgr_platform_shutdown(void) {}//TODO call grpc_pollset_global_shutdown}

static grpc_iomgr_platform_vtable vtable = {
    iomgr_platform_init, iomgr_platform_flush, iomgr_platform_shutdown};

grpc_iomgr_platform_vtable* custom_iomgr_platform_vtable() {
  return &vtable;
}
