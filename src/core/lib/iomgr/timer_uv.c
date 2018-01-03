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

#if GRPC_UV

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include "src/core/lib/debug/trace.h"
#include "src/core/lib/iomgr/iomgr_custom.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/iomgr/timer_custom.h"

#include <uv.h>

static void timer_close_callback(uv_handle_t *handle) { gpr_free(handle); }

static void stop_uv_timer(uv_timer_t *handle) {
  uv_timer_stop(handle);
  uv_unref((uv_handle_t *)handle);
  uv_close((uv_handle_t *)handle, timer_close_callback);
}

void run_expired_timer(uv_timer_t *handle) {
  grpc_timer_wrapper *timer_wrapper = (grpc_timer_wrapper *)handle->data;
  stop_uv_timer(handle);
  grpc_custom_timer_callback(timer_wrapper, GRPC_ERROR_NONE);
}

static grpc_error* timer_start(grpc_timer_wrapper* t) {
  uv_timer_t* uv_timer;
  uv_timer = gpr_malloc(sizeof(uv_timer_t));
  uv_timer_init(uv_default_loop(), uv_timer);
  uv_timer->data = t;
  t->timer = (void*) uv_timer;
  uv_timer_start(uv_timer, run_expired_timer, t->timeout_ms, 0);
  /* We assume that gRPC timers are only used alongside other active gRPC
     objects, and that there will therefore always be something else keeping
     the uv loop alive whenever there is a timer */
  uv_unref((uv_handle_t *)uv_timer);
  return GRPC_ERROR_NONE;
}

static grpc_error* timer_stop(grpc_timer_wrapper* t) {
  stop_uv_timer((uv_timer_t *)t->timer);
  return GRPC_ERROR_NONE;
}

grpc_custom_timer_vtable uv_timer_vtable = {timer_start, timer_stop};

#endif
