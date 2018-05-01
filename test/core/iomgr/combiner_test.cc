/*
 *
 * Copyright 2015 gRPC authors.
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

#include "src/core/lib/iomgr/combiner.h"

#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include "src/core/lib/gpr/useful.h"
#include "src/core/lib/gprpp/thd.h"
#include "test/core/util/test_config.h"

void test_func(void* arg, grpc_error* error) {
  int* tmp = (int*)arg;
  *tmp = *tmp + 1;
}

int main(int argc, char** argv) {
  grpc_test_init(argc, argv);
  grpc_init();
  {
  grpc_core::ExecCtx exec_ctx;
  grpc_closure test_closure;
  //grpc_combiner* lock = grpc_combiner_create();
  int tmp = 0;
  int i = 0;
  for (i = 0; i < 100000000; i++) {
    //test_func((void*)&tmp, GRPC_ERROR_NONE);
    GRPC_CLOSURE_INIT(&test_closure, test_func, (void*)&tmp, grpc_schedule_on_exec_ctx);
    GRPC_CLOSURE_SCHED(&test_closure, GRPC_ERROR_NONE);
    exec_ctx.Flush();
  }
  }

  grpc_shutdown();

  return 0;
}

