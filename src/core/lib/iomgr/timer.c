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

#include "src/core/lib/iomgr/timer.h"


extern grpc_timer_vtable grpc_default_timer_vtable;
static grpc_timer_vtable* timer_vtable = NULL;

void grpc_set_timer_impl(grpc_timer_vtable* vtable) {
  timer_vtable = vtable;
}

void grpc_global_timer_init() {
  if (timer_vtable == NULL) {
    timer_vtable = &grpc_default_timer_vtable;
  }
}

void grpc_timer_init(grpc_exec_ctx *exec_ctx, grpc_timer *timer,
                     gpr_timespec deadline, grpc_closure *closure,
                     gpr_timespec now) {
  timer_vtable->init(exec_ctx, timer, deadline, closure, now);
}

void grpc_timer_cancel(grpc_exec_ctx *exec_ctx, grpc_timer *timer) {
  timer_vtable->cancel(exec_ctx, timer);
}

grpc_timer_check_result grpc_timer_check(grpc_exec_ctx *exec_ctx, gpr_timespec now,
				         gpr_timespec *next) {
  return timer_vtable->check(exec_ctx, now, next);
}

void grpc_timer_list_init(gpr_timespec now) {
  timer_vtable->list_init(now);
}

void grpc_timer_list_shutdown(grpc_exec_ctx *exec_ctx) {
  timer_vtable->list_shutdown(exec_ctx);
}

void grpc_timer_consume_kick() {
  timer_vtable->consume_kick();
}
