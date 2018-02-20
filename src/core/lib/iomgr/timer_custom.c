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

#include "src/core/lib/iomgr/port.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include "src/core/lib/debug/trace.h"
#include "src/core/lib/iomgr/iomgr_custom.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/iomgr/timer_custom.h"

static grpc_custom_timer_vtable* custom_timer_impl = NULL;

void grpc_custom_timer_callback(grpc_timer_wrapper* t, grpc_error* error) {
  grpc_timer *timer = t->original;
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  GRPC_UV_ASSERT_SAME_THREAD();
  GPR_ASSERT(timer->pending);
  timer->pending = 0;
  GRPC_CLOSURE_SCHED(&exec_ctx, timer->closure, GRPC_ERROR_NONE);
  grpc_exec_ctx_finish(&exec_ctx);
  custom_timer_impl->stop(t);
  gpr_free(t);
}

static void timer_init(grpc_exec_ctx *exec_ctx, grpc_timer *timer,
                     gpr_timespec deadline, grpc_closure *closure,
                     gpr_timespec now) {
  uint64_t timeout;
  GRPC_UV_ASSERT_SAME_THREAD();
  if (gpr_time_cmp(deadline, now) <= 0) {
    GRPC_CLOSURE_SCHED(exec_ctx, closure, GRPC_ERROR_NONE);
    timer->pending = false;
    return;
  }
  gpr_log(GPR_ERROR, "TIMER ON POLLSET");
  timer->pending = true;
  timer->closure = closure;
  grpc_timer_wrapper* timer_wrapper = (grpc_timer_wrapper*) gpr_malloc(sizeof(grpc_timer_wrapper));
  timeout = (uint64_t)gpr_time_to_millis(gpr_time_sub(deadline, now));
  timer_wrapper->timeout_ms = timeout;
  timer->custom_timer = (void*) timer_wrapper;
  timer_wrapper->original = timer;
  custom_timer_impl->start(timer_wrapper);
}

static void timer_cancel(grpc_exec_ctx *exec_ctx, grpc_timer *timer) {
  GRPC_UV_ASSERT_SAME_THREAD();
  gpr_log(GPR_ERROR, "TIMER CANCEL");
  grpc_timer_wrapper* tw = (grpc_timer_wrapper*) timer->custom_timer;
  if (timer->pending) {
    timer->pending = 0;
    GRPC_CLOSURE_SCHED(exec_ctx, timer->closure, GRPC_ERROR_CANCELLED);
    custom_timer_impl->stop(tw);
    gpr_free(tw);
  }
}

static grpc_timer_check_result timer_check(grpc_exec_ctx *exec_ctx,
                                    gpr_timespec now, gpr_timespec *next) {
  return GRPC_TIMERS_NOT_CHECKED;
}

static void timer_list_init(gpr_timespec now) {}
static void timer_list_shutdown(grpc_exec_ctx *exec_ctx) {}

static void timer_consume_kick(void) {}

static grpc_timer_vtable custom_timer_vtable = {timer_init, timer_cancel, timer_check, timer_list_init,
                                               timer_list_shutdown, timer_consume_kick};

void grpc_custom_timer_init(grpc_custom_timer_vtable* impl) {
  custom_timer_impl = impl;
  grpc_set_timer_impl(&custom_timer_vtable);
}

