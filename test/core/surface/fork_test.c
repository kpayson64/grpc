/*
 *
 * Copyright 2017, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <grpc/byte_buffer.h>
#include <grpc/fork.h>
#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/host_port.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>
#include <grpc/support/useful.h>

#include "src/core/lib/support/fork.h"
#include "src/core/lib/support/string.h"
#include "test/core/util/port.h"

#define CONCURRENT_REQUESTS 4
#define CASCADE_FORK_DEPTH 10
#define DEFAULT_DEADLINE_SEC 15

/*******************************************************************************
 * Test Server/Client utility functions
 */

typedef struct test_channel {
  grpc_channel* channel;
  grpc_completion_queue* cq;
} test_channel;

typedef struct test_server {
  grpc_server* server;
  grpc_completion_queue* cq;
  grpc_completion_queue* notify;
} test_server;

static gpr_timespec default_deadline(void) {
  return gpr_time_add(
      gpr_now(GPR_CLOCK_REALTIME),
      gpr_time_from_seconds(DEFAULT_DEADLINE_SEC, GPR_TIMESPAN));
}

static test_server* create_test_server(int port, int multi_port) {
  test_server* server = gpr_malloc(sizeof(test_server));
  char* localaddr;
  gpr_join_host_port(&localaddr, "localhost", port);

  grpc_arg so_reuseport;
  so_reuseport.type = GRPC_ARG_INTEGER;
  so_reuseport.key = GRPC_ARG_ALLOW_REUSEPORT;
  so_reuseport.value.integer = multi_port;
  grpc_channel_args args;
  args.num_args = 1;
  args.args = &so_reuseport;

  server->server = grpc_server_create(&args, NULL);
  server->cq = grpc_completion_queue_create(NULL);
  server->notify = grpc_completion_queue_create(NULL);

  grpc_server_register_completion_queue(server->server, server->notify, NULL);
  GPR_ASSERT(grpc_server_add_insecure_http2_port(server->server, localaddr));
  gpr_free(localaddr);

  grpc_server_start(server->server);
  return server;
}

static test_channel* create_test_channel(int port, int idx) {
  test_channel* channel = gpr_malloc(sizeof(test_channel));
  char* localaddr;
  gpr_join_host_port(&localaddr, "localhost", port);

  // We set the arg to ensure channels don't point to
  // the same TCP connection
  grpc_arg channel_idx;
  channel_idx.type = GRPC_ARG_INTEGER;
  channel_idx.key = "grpc.channel.index";
  channel_idx.value.integer = idx;
  grpc_channel_args args;
  args.num_args = 1;
  args.args = &channel_idx;

  channel->channel = grpc_insecure_channel_create(localaddr, &args, NULL);
  channel->cq = grpc_completion_queue_create(NULL);
  gpr_free(localaddr);
  return channel;
}

static void destroy_test_channel(test_channel* channel) {
  grpc_channel_destroy(channel->channel);
  grpc_completion_queue_destroy(channel->cq);
  gpr_free(channel);
}

static void destroy_test_server(test_server* server) {
  grpc_server_shutdown_and_notify(server->server, server->notify, NULL);
  GPR_ASSERT(
      grpc_completion_queue_next(server->notify, default_deadline(), NULL)
          .success);
  grpc_server_destroy(server->server);
  grpc_completion_queue_destroy(server->notify);
  grpc_completion_queue_destroy(server->cq);
  gpr_free(server);
}

static void await_exit(int pid) {
  int status = 0;
  waitpid(pid, &status, 0);
  GPR_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/*******************************************************************************
 * Test Request/Response functions
 */
static grpc_call* start_streaming_request(test_channel* channel) {
  grpc_call* c;
  gpr_timespec deadline = default_deadline();
  grpc_op ops[6];
  grpc_op* op;
  grpc_metadata_array initial_metadata_recv;
  grpc_call_error error;
  c = grpc_channel_create_call(
      channel->channel, NULL, GRPC_PROPAGATE_DEFAULTS, channel->cq,
      grpc_slice_from_static_string("/foo"), NULL, deadline, NULL);
  GPR_ASSERT(c);

  grpc_metadata_array_init(&initial_metadata_recv);

  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  op->op = GRPC_OP_RECV_INITIAL_METADATA;
  op->data.recv_initial_metadata.recv_initial_metadata = &initial_metadata_recv;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  error = grpc_call_start_batch(c, ops, (size_t)(op - ops), NULL, NULL);
  GPR_ASSERT(GRPC_CALL_OK == error);
  GPR_ASSERT(grpc_completion_queue_next(channel->cq, default_deadline(), NULL)
                 .success);
  grpc_metadata_array_destroy(&initial_metadata_recv);

  return c;
}

static void finish_streaming_request(test_channel* channel, bool disconnected,
                                     grpc_call* c) {
  grpc_op ops[6];
  grpc_op* op;
  grpc_metadata_array trailing_metadata_recv;
  grpc_status_code status;
  grpc_call_error error;
  grpc_slice details;
  grpc_metadata_array_init(&trailing_metadata_recv);
  grpc_slice request_slice = grpc_slice_from_copied_string("hello");
  grpc_byte_buffer* request = grpc_raw_byte_buffer_create(&request_slice, 1);

  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_SEND_MESSAGE;
  op->data.send_message.send_message = request;
  op++;
  op->op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  op->data.recv_status_on_client.trailing_metadata = &trailing_metadata_recv;
  op->data.recv_status_on_client.status = &status;
  op->data.recv_status_on_client.status_details = &details;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  error = grpc_call_start_batch(c, ops, (size_t)(op - ops), NULL, NULL);
  GPR_ASSERT(GRPC_CALL_OK == error);

  GPR_ASSERT(grpc_completion_queue_next(channel->cq, default_deadline(), NULL)
                 .success);
  if (disconnected) {
    GPR_ASSERT(status == GRPC_STATUS_UNAVAILABLE);
  } else {
    GPR_ASSERT(status == GRPC_STATUS_UNIMPLEMENTED);
    GPR_ASSERT(0 == grpc_slice_str_cmp(details, "xyz"));
  }

  grpc_slice_unref(details);
  grpc_metadata_array_destroy(&trailing_metadata_recv);
  grpc_byte_buffer_destroy(request);

  grpc_call_destroy(c);
}

static grpc_call* start_streaming_response(test_server* server) {
  grpc_call* s;
  grpc_op ops[6];
  grpc_op* op;
  grpc_call_error error;
  grpc_call_details call_details;
  grpc_metadata_array request_metadata_recv;

  grpc_metadata_array_init(&request_metadata_recv);
  grpc_call_details_init(&call_details);

  error = grpc_server_request_call(server->server, &s, &call_details,
                                   &request_metadata_recv, server->cq,
                                   server->notify, NULL);
  GPR_ASSERT(GRPC_CALL_OK == error);
  GPR_ASSERT(
      grpc_completion_queue_next(server->notify, default_deadline(), NULL)
          .success);

  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  error = grpc_call_start_batch(s, ops, (size_t)(op - ops), NULL, NULL);
  GPR_ASSERT(
      grpc_completion_queue_next(server->cq, default_deadline(), NULL).success);
  GPR_ASSERT(GRPC_CALL_OK == error);
  GPR_ASSERT(0 == grpc_slice_str_cmp(call_details.method, "/foo"));
  GPR_ASSERT(0 == call_details.flags);

  grpc_metadata_array_destroy(&request_metadata_recv);
  grpc_call_details_destroy(&call_details);
  return s;
}

static void finish_streaming_response(test_server* server, grpc_call* s) {
  grpc_op ops[6];
  grpc_op* op;
  int was_cancelled = 2;
  grpc_byte_buffer* response = NULL;

  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_RECV_MESSAGE;
  op->data.recv_message.recv_message = &response;
  op++;
  op->op = GRPC_OP_RECV_CLOSE_ON_SERVER;
  op->data.recv_close_on_server.cancelled = &was_cancelled;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  op->op = GRPC_OP_SEND_STATUS_FROM_SERVER;
  op->data.send_status_from_server.trailing_metadata_count = 0;
  op->data.send_status_from_server.status = GRPC_STATUS_UNIMPLEMENTED;
  grpc_slice status_details = grpc_slice_from_static_string("xyz");
  op->data.send_status_from_server.status_details = &status_details;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  grpc_call_start_batch(s, ops, (size_t)(op - ops), NULL, NULL);
  GPR_ASSERT(
      grpc_completion_queue_next(server->cq, default_deadline(), NULL).success);

  grpc_byte_buffer_destroy(response);
  grpc_call_destroy(s);
}

static void send_unary_request(test_channel* channel, int expect_unavailable) {
  grpc_call* c;
  gpr_timespec deadline = default_deadline();
  grpc_op ops[6];
  grpc_op* op;
  grpc_metadata_array initial_metadata_recv;
  grpc_metadata_array trailing_metadata_recv;
  grpc_status_code status;
  grpc_call_error error;
  grpc_slice details;
  c = grpc_channel_create_call(
      channel->channel, NULL, GRPC_PROPAGATE_DEFAULTS, channel->cq,
      grpc_slice_from_static_string("/foo"), NULL, deadline, NULL);
  GPR_ASSERT(c);

  grpc_metadata_array_init(&initial_metadata_recv);
  grpc_metadata_array_init(&trailing_metadata_recv);

  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  op->op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  op->op = GRPC_OP_RECV_INITIAL_METADATA;
  op->data.recv_initial_metadata.recv_initial_metadata = &initial_metadata_recv;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  op->data.recv_status_on_client.trailing_metadata = &trailing_metadata_recv;
  op->data.recv_status_on_client.status = &status;
  op->data.recv_status_on_client.status_details = &details;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  error = grpc_call_start_batch(c, ops, (size_t)(op - ops), NULL, NULL);
  GPR_ASSERT(GRPC_CALL_OK == error);

  GPR_ASSERT(grpc_completion_queue_next(channel->cq, default_deadline(), NULL)
                 .success);
  if (!expect_unavailable) {
    GPR_ASSERT(status == GRPC_STATUS_UNIMPLEMENTED);
    GPR_ASSERT(0 == grpc_slice_str_cmp(details, "xyz"));
  } else {
    GPR_ASSERT(status == GRPC_STATUS_UNAVAILABLE);
  }

  grpc_slice_unref(details);
  grpc_metadata_array_destroy(&initial_metadata_recv);
  grpc_metadata_array_destroy(&trailing_metadata_recv);

  grpc_call_destroy(c);
}

static void send_unary_response(test_server* server) {
  grpc_call* s;
  grpc_op ops[6];
  grpc_op* op;
  grpc_call_error error;
  grpc_call_details call_details;
  int was_cancelled = 2;
  grpc_metadata_array request_metadata_recv;

  grpc_metadata_array_init(&request_metadata_recv);
  grpc_call_details_init(&call_details);

  error = grpc_server_request_call(server->server, &s, &call_details,
                                   &request_metadata_recv, server->cq,
                                   server->notify, NULL);
  GPR_ASSERT(GRPC_CALL_OK == error);
  GPR_ASSERT(
      grpc_completion_queue_next(server->notify, default_deadline(), NULL)
          .success);

  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  op->op = GRPC_OP_SEND_STATUS_FROM_SERVER;
  op->data.send_status_from_server.trailing_metadata_count = 0;
  op->data.send_status_from_server.status = GRPC_STATUS_UNIMPLEMENTED;
  grpc_slice status_details = grpc_slice_from_static_string("xyz");
  op->data.send_status_from_server.status_details = &status_details;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  op->op = GRPC_OP_RECV_CLOSE_ON_SERVER;
  op->data.recv_close_on_server.cancelled = &was_cancelled;
  op->flags = 0;
  op->reserved = NULL;
  op++;
  error = grpc_call_start_batch(s, ops, (size_t)(op - ops), NULL, NULL);
  GPR_ASSERT(
      grpc_completion_queue_next(server->cq, default_deadline(), NULL).success);
  GPR_ASSERT(GRPC_CALL_OK == error);
  GPR_ASSERT(0 == grpc_slice_str_cmp(call_details.method, "/foo"));
  GPR_ASSERT(0 == call_details.flags);

  grpc_metadata_array_destroy(&request_metadata_recv);
  grpc_call_details_destroy(&call_details);
  grpc_call_destroy(s);
}

/*******************************************************************************
 * Test Definitions
 */

static void run_prefork_server(test_server* server, bool streaming,
                               int num_requests, int num_handlers, int* pids) {
  for (int i = 0; i < num_handlers; i++) {
    grpc_prefork();
    int pid = fork();
    if (pid) {
      pids[i] = pid;
    } else {
      grpc_postfork_child();
      for (int j = 0; j < num_requests; j++) {
        if (streaming) {
          grpc_call* call = start_streaming_response(server);
          finish_streaming_response(server, call);
        } else {
          send_unary_response(server);
        }
      }
      destroy_test_server(server);
      exit(0);
    }
  }
}

static void test_prefork_server_single_handler() {
  int port = grpc_pick_unused_port_or_die();
  test_server* server = create_test_server(port, 0);
  test_channel* channel = create_test_channel(port, 0);
  int pid;
  run_prefork_server(server, false, 1, 1, &pid);
  send_unary_request(channel, 0);
  await_exit(pid);
  destroy_test_channel(channel);
  destroy_test_server(server);
}

static void test_prefork_server_so_reuseport() {
  int port = grpc_pick_unused_port_or_die();
  test_server* server = create_test_server(port, 1);
  test_channel* channel[CONCURRENT_REQUESTS];
  for (int i = 0; i < CONCURRENT_REQUESTS; i++) {
    channel[i] = create_test_channel(port, i);
  }

  int server_pids[CONCURRENT_REQUESTS];
  run_prefork_server(server, true, 1, CONCURRENT_REQUESTS, server_pids);
  // Destroy the server in the parent process, otherwise
  // SO_REUSEPORT can cause it to pick up incoming connections
  destroy_test_server(server);

  for (int i = 0; i < CONCURRENT_REQUESTS; i++) {
    send_unary_request(channel[i], 0);
    // Wait for the subprocess that handled this request to exit
    // We want to ensure each subprocess can pick up requests
    sleep(1);
  }

  for (int i = 0; i < CONCURRENT_REQUESTS; i++) {
    await_exit(server_pids[i]);
    destroy_test_channel(channel[i]);
  }
}

// Forks seperate server processes, and test that
// each process can serve a streaming request concurrently
static void test_prefork_server_multi_handlers() {
  int port = grpc_pick_unused_port_or_die();
  test_server* server = create_test_server(port, 0);
  test_channel* channel[CONCURRENT_REQUESTS];
  for (int i = 0; i < CONCURRENT_REQUESTS; i++) {
    channel[i] = create_test_channel(port, i);
  }

  int server_pids[CONCURRENT_REQUESTS];
  grpc_call* calls[CONCURRENT_REQUESTS];
  run_prefork_server(server, true, 1, CONCURRENT_REQUESTS, server_pids);
  for (int i = 0; i < CONCURRENT_REQUESTS; i++) {
    calls[i] = start_streaming_request(channel[i]);
    // Servers will greedily accept() as many connections as possible
    // This ensures each child server gets exactly one connection
    sleep(1);
  }
  for (int i = 0; i < CONCURRENT_REQUESTS; i++) {
    finish_streaming_request(channel[i], false, calls[i]);
  }

  for (int i = 0; i < CONCURRENT_REQUESTS; i++) {
    await_exit(server_pids[i]);
    destroy_test_channel(channel[i]);
  }
}

// Process forks, then both parent + child make a request
static void test_fork_before_connect() {
  int port = grpc_pick_unused_port_or_die();
  test_server* server = create_test_server(port, 0);
  test_channel* channel = create_test_channel(port, 0);

  int server_pid;
  run_prefork_server(server, false, 2, 1, &server_pid);

  grpc_prefork();
  int pid = fork();
  if (pid) {
    send_unary_request(channel, 0);
    await_exit(pid);
    await_exit(server_pid);
  } else {
    grpc_postfork_child();
    send_unary_request(channel, 0);
    exit(0);
  }
  destroy_test_channel(channel);
}

// Makes a request on a channel, then forks, and tests
// that both the parent and child can re-use that channel
static void test_fork_after_connect() {
  int port = grpc_pick_unused_port_or_die();
  test_server* server = create_test_server(port, 0);
  test_channel* channel = create_test_channel(port, 0);

  int server_pid;
  run_prefork_server(server, false, 3, 1, &server_pid);
  send_unary_request(channel, 0);

  grpc_prefork();
  int pid = fork();
  if (pid) {
    send_unary_request(channel, 0);
    await_exit(pid);
    await_exit(server_pid);
  } else {
    grpc_postfork_child();
    // The first request should fail with UNAVAILABLE
    send_unary_request(channel, 1);
    send_unary_request(channel, 0);
    exit(0);
  }
  destroy_test_channel(channel);
}

// Each child process makes a request, and forks a new child
static void test_fork_after_connect_cascade() {
  int port = grpc_pick_unused_port_or_die();
  test_server* server = create_test_server(port, 0);
  test_channel* channel = create_test_channel(port, 0);

  int server_pid;
  run_prefork_server(server, false, 2 * CASCADE_FORK_DEPTH + 1, 1, &server_pid);
  send_unary_request(channel, 0);

  int root_process = 1;
  for (int i = 0; i < CASCADE_FORK_DEPTH; i++) {
    grpc_prefork();
    int pid = fork();
    if (pid) {
      send_unary_request(channel, 0);
      destroy_test_channel(channel);
      await_exit(pid);
      break;
    } else {
      root_process = 0;
      grpc_postfork_child();
      send_unary_request(channel, 1);
      send_unary_request(channel, 0);
    }
  }

  if (root_process) {
    await_exit(server_pid);
  } else {
    exit(0);
  }
}

int main(int argc, char** argv) {
  grpc_init();
  grpc_enable_fork_support(1);
  test_prefork_server_so_reuseport();
  test_prefork_server_single_handler();
  test_fork_before_connect();
  test_fork_after_connect_cascade();
  test_prefork_server_single_handler();
  test_fork_before_connect();
  test_fork_after_connect();
  test_prefork_server_multi_handlers();
  test_fork_after_connect();
  grpc_shutdown();
  return 0;
}
