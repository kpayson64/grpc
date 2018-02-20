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

#ifdef GRPC_UV

#include "src/core/lib/iomgr/pollset_custom.h"

#include <uv.h>

static void run_loop(int blocking) {
  if(blocking) {
    uv_run(uv_default_loop(), UV_RUN_ONCE);
  } else {
    uv_run(uv_default_loop(), UV_RUN_NOWAIT);
  }
}

grpc_custom_poller_vtable uv_pollset_vtable = {run_loop};

#endif /* GRPC_UV */
