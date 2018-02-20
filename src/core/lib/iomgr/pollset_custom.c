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

#include <string.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/sync.h>

#include "src/core/lib/iomgr/closure.h"
#include "src/core/lib/iomgr/iomgr_custom.h"
#include "src/core/lib/iomgr/pollset.h"
#include "src/core/lib/iomgr/pollset_custom.h"
#include "src/core/lib/iomgr/timer.h"

#include "src/core/lib/debug/trace.h"

/*
#ifndef NDEBUG
grpc_tracer_flag grpc_trace_fd_refcount =
    GRPC_TRACER_INITIALIZER(false, "fd_refcount");
#endif
*/

static grpc_custom_poller_vtable* poller_vtable;

static gpr_mu grpc_polling_mu;

// Timer used to kick the event loop
static grpc_timer g_kick_timer;
static grpc_timer g_loop_timer;
static int kick_count;
static gpr_cv no_kicks;
static grpc_closure g_kick_closure;
static grpc_closure g_timer_closure;


// This needs to be non-zero to ensure we don't get any NULL pollsets
static size_t pollset_size() { return 1;}

static void dummy_cb(grpc_exec_ctx* exec_ctx, void* arg, grpc_error* error) {}

static void dummy_kick_cb(grpc_exec_ctx* exec_ctx, void* arg, grpc_error* error) {
  gpr_log(GPR_ERROR, "POLLSET KICKED CB");
  kick_count--;
  if (kick_count == 0) {
    gpr_cv_signal(&no_kicks);
  }
}

static void pollset_global_init() {
  gpr_mu_init(&grpc_polling_mu);
  gpr_cv_init(&no_kicks);
  kick_count = 0;
  GRPC_CLOSURE_INIT(&g_kick_closure, dummy_kick_cb, NULL, grpc_schedule_on_exec_ctx);
  GRPC_CLOSURE_INIT(&g_timer_closure, dummy_cb, NULL, grpc_schedule_on_exec_ctx);
}

static void pollset_global_shutdown(void) {
  GRPC_UV_ASSERT_SAME_THREAD();
  gpr_mu_lock(&grpc_polling_mu);
  if (kick_count > 0) {
    gpr_cv_wait(&no_kicks, &grpc_polling_mu, gpr_inf_future(GPR_CLOCK_REALTIME));
  }
  gpr_mu_unlock(&grpc_polling_mu);
  gpr_mu_destroy(&grpc_polling_mu);
}

static void pollset_init(grpc_pollset *pollset, gpr_mu **mu) {
  GRPC_UV_ASSERT_SAME_THREAD();
  *mu = &grpc_polling_mu;
}

static void pollset_shutdown(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset,
                           grpc_closure *closure) {
  GRPC_UV_ASSERT_SAME_THREAD();
  // Drain any pending work without blocking
  gpr_log(GPR_ERROR, "POLLSET SHUTDOWN CALLED");
  poller_vtable->run_loop(0);
  GRPC_CLOSURE_SCHED(exec_ctx, closure, GRPC_ERROR_NONE);
}

static void pollset_destroy(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset) {
  GRPC_UV_ASSERT_SAME_THREAD();
}

static grpc_error *pollset_work(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset,
                              grpc_pollset_worker **worker_hdl,
                              gpr_timespec now, gpr_timespec deadline) {
  gpr_log(GPR_ERROR, "POLLSET WORK CALLED");
  GRPC_UV_ASSERT_SAME_THREAD();
  gpr_mu_unlock(&grpc_polling_mu);
    if (gpr_time_cmp(deadline, now) > 0) {
      grpc_timer_init(exec_ctx, &g_loop_timer, deadline, &g_timer_closure, now);
      /* Run until there is some I/O activity or the timer triggers. It doesn't
         matter which happens */
      poller_vtable->run_loop(1);
      grpc_timer_cancel(exec_ctx, &g_loop_timer);
    } else {
      poller_vtable->run_loop(0);
    }
  if (!grpc_closure_list_empty(exec_ctx->closure_list)) {
    grpc_exec_ctx_flush(exec_ctx);
  }
  gpr_mu_lock(&grpc_polling_mu);
  return GRPC_ERROR_NONE;
}

static grpc_error *pollset_kick(grpc_pollset *pollset,
                              grpc_pollset_worker *specific_worker) {
  GRPC_UV_ASSERT_SAME_THREAD();
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  // We already have a kick qued up
  if (kick_count > 0) {
    return GRPC_ERROR_NONE;
  }
  gpr_log(GPR_ERROR, "POLLSET KICKED");
  kick_count++;
  // Make sure now != deadline so this ends up on the event loop
  gpr_timespec now = gpr_now(GPR_CLOCK_REALTIME);
  gpr_timespec deadline = gpr_time_add(
        now,
        gpr_time_from_micros(1, GPR_TIMESPAN));
  grpc_timer_init(&exec_ctx, &g_kick_timer, deadline, &g_kick_closure, now);
  return GRPC_ERROR_NONE;
}

grpc_pollset_vtable custom_pollset_vtable = {
  pollset_global_init, pollset_global_shutdown, pollset_init,
  pollset_shutdown, pollset_destroy, pollset_work, pollset_kick, pollset_size};

void grpc_custom_pollset_init(grpc_custom_poller_vtable* vtable) {
  poller_vtable = vtable;
  grpc_set_pollset_vtable(&custom_pollset_vtable);
}
