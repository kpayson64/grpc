/*
 *
 * Copyright 2018 gRPC authors.
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

#include "src/core/ext/transport/nghttp2/nghttp2_transport.h"

#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include "src/core/lib/gpr/useful.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"

#include "src/core/lib/gpr/string.h"
#include "test/core/util/slice_splitter.h"
#include "test/core/util/test_config.h"

#include <gtest/gtest.h>

namespace grpc_core {
namespace nghttp2 {
namespace {

//
// Nghttp2 tests
//

TEST(ClientTransport, UpDown) {
 grpc_core::ExecCtx exec_ctx;
 ClientTransport client;
}

TEST(ClientTransport, Flush) {
 grpc_core::ExecCtx exec_ctx;
 grpc_slice_buffer buffer;
 grpc_slice_buffer_init(&buffer);
 ClientTransport client;
 client.Flush(&buffer);
 grpc_slice_buffer_destroy_internal(&buffer);
}

TEST(ClientTransport, Shutdown) {
 grpc_core::ExecCtx exec_ctx;
 grpc_slice_buffer buffer;
 grpc_slice_buffer_init(&buffer);
 ClientTransport client;
 client.Shutdown(GRPC_ERROR_CREATE_FROM_STATIC_STRING("Shutdown error"));
 client.Flush(&buffer);
 grpc_slice_buffer_destroy_internal(&buffer);
}

TEST(ClientTransport, CreateStream) {
 grpc_core::ExecCtx exec_ctx;
 grpc_slice_buffer buffer;
 grpc_slice_buffer_init(&buffer);
 ClientTransport* client = grpc_core::New<ClientTransport>();
 Stream stream(client->Ref(), -1);
 client->Flush(&buffer);
 client->Unref();
 grpc_slice_buffer_destroy_internal(&buffer);
}

TEST(ServerTransport, UpDown) {
 grpc_core::ExecCtx exec_ctx;
 ServerTransport server;
}

TEST(ServerTransport, Flush) {
 grpc_core::ExecCtx exec_ctx;
 grpc_slice_buffer buffer;
 grpc_slice_buffer_init(&buffer);
 ServerTransport server;
 server.Flush(&buffer);
 grpc_slice_buffer_destroy_internal(&buffer);
}

TEST(ServerTransport, Shutdown) {
 grpc_core::ExecCtx exec_ctx;
 grpc_slice_buffer buffer;
 grpc_slice_buffer_init(&buffer);
 ServerTransport server;
 server.Shutdown(GRPC_ERROR_CREATE_FROM_STATIC_STRING("Shutdown error"));
 server.Flush(&buffer);
 grpc_slice_buffer_destroy_internal(&buffer);
}

TEST(TransportE2E, Connect) {
 grpc_core::ExecCtx exec_ctx;
 grpc_slice_buffer buffer;
 grpc_slice_buffer_init(&buffer);
 ClientTransport client;
 ServerTransport server;
 client.Flush(&buffer);
 GPR_ASSERT(buffer.length > 0);
 server.Read(&buffer);
 grpc_slice_buffer_reset_and_unref(&buffer);
 server.Flush(&buffer);
 GPR_ASSERT(buffer.length > 0);
 grpc_slice_buffer_destroy_internal(&buffer);
}

TEST(TransportE2E, ClientShutdownIdleConnection) {
 grpc_core::ExecCtx exec_ctx;
 grpc_slice_buffer buffer;
 grpc_slice_buffer_init(&buffer);
 ClientTransport client;
 ServerTransport server;
 client.Flush(&buffer);
 GPR_ASSERT(buffer.length > 0);
 server.Read(&buffer);
 grpc_slice_buffer_reset_and_unref(&buffer);
 server.Flush(&buffer);
 GPR_ASSERT(buffer.length > 0);
 grpc_slice_buffer_reset_and_unref(&buffer);
 client.Shutdown(GRPC_ERROR_CREATE_FROM_STATIC_STRING("Shutdown error"));
 client.Flush(&buffer);
 grpc_slice_buffer_reset_and_unref(&buffer);
 grpc_slice_buffer_destroy_internal(&buffer);
}

static bool md_is_eq(grpc_metadata_batch* mb1, grpc_metadata_batch* mb2) {
  bool is_equal = true;
  for (grpc_linked_mdelem* m1 = mb1->list.head; m1 !=nullptr; m1 = m1->next) {
    bool matched = false;
    for (grpc_linked_mdelem* m2 = mb2->list.head; m2 !=nullptr; m2 = m2->next) {
      if (grpc_slice_eq(GRPC_MDKEY(m1->md), GRPC_MDKEY(m2->md))) {
        matched = true;
        if (!grpc_slice_eq(GRPC_MDVALUE(m1->md), GRPC_MDVALUE(m2->md))) {
          char* key = grpc_slice_to_c_string(GRPC_MDKEY(m1->md));
          char* v1 = grpc_slice_to_c_string(GRPC_MDVALUE(m1->md));
          char* v2 = grpc_slice_to_c_string(GRPC_MDVALUE(m2->md));
          gpr_log(GPR_ERROR, "Metadata values didn't match: Key %s, Value1: %s, Value2: %s", key, v1, v2);
          gpr_free(key);
          gpr_free(v1);
          gpr_free(v2);
          is_equal = false;
        }
      }
    }
    if (!matched) {
      char* key = grpc_slice_to_c_string(GRPC_MDKEY(m1->md));
      gpr_log(GPR_ERROR, "Metadata key missing %s", key);
      gpr_free(key);
      is_equal = false;
    }
  }

  for (grpc_linked_mdelem* m2 = mb2->list.head; m2 !=nullptr; m2 = m2->next) {
    bool matched = false;
    for (grpc_linked_mdelem* m1 = mb1->list.head; m1 !=nullptr; m1 = m1->next) {
      if (grpc_slice_eq(GRPC_MDKEY(m1->md), GRPC_MDKEY(m2->md))) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      char* key = grpc_slice_to_c_string(GRPC_MDKEY(m2->md));
      gpr_log(GPR_ERROR, "Metadata key missing %s", key);
      gpr_free(key);
      is_equal = false;
    }
  }
  return is_equal;
}

static bool msg_is_eq(grpc_core::OrphanablePtr<grpc_core::ByteStream>* msg, grpc_slice exp) {
  grpc_slice_buffer msg_buffer;
  grpc_slice_buffer_init(&msg_buffer);

  bool is_equal = true;
  size_t bytes_read = 0;
  while(bytes_read < (*msg)->length()) {
    grpc_slice slice;
    GPR_ASSERT((*msg)->Pull(&slice) == GRPC_ERROR_NONE);
    grpc_slice_buffer_add(&msg_buffer, slice);
    bytes_read += GRPC_SLICE_LENGTH(slice);
  }

  grpc_slice actual = grpc_slice_merge(msg_buffer.slices, msg_buffer.count);

  if(!grpc_slice_eq(actual, exp)) {
    char* actual_str = grpc_dump_slice(actual, GPR_DUMP_HEX | GPR_DUMP_ASCII);
    char* exp_str = grpc_dump_slice(exp, GPR_DUMP_HEX | GPR_DUMP_ASCII);
    gpr_log(GPR_ERROR, "Messages not equal");
    gpr_log(GPR_ERROR, "EXPECT: %s", exp_str);
    gpr_log(GPR_ERROR, "GOT:    %s", actual_str);
    gpr_free(actual_str);
    gpr_free(exp_str);
    is_equal = false;
  }
  grpc_slice_unref_internal(actual);
  grpc_slice_buffer_destroy(&msg_buffer);
  return is_equal;
}

static void on_initial_md(void* arg, grpc_error* error);
static void on_message(void* arg, grpc_error* error);
static void on_close(void* arg, grpc_error* error);


struct StreamWrapper {
  StreamWrapper(Stream* stream) {
    grpc_metadata_batch_init(&initial_md_recv);
    grpc_metadata_batch_init(&trailing_md_recv);
    GRPC_CLOSURE_INIT(&recv_initial_md_cb, on_initial_md, this, grpc_schedule_on_exec_ctx);
    GRPC_CLOSURE_INIT(&recv_msg_cb, on_message, this, grpc_schedule_on_exec_ctx);
    GRPC_CLOSURE_INIT(&recv_trailing_md_cb, on_close, this, grpc_schedule_on_exec_ctx);
    initial_md_cb_called = false;
    msg_cb_called = false;
    trailing_md_cb_called = false;
    recv_initial_md_error = GRPC_ERROR_NONE;
    recv_msg_cb_error = GRPC_ERROR_NONE;
    recv_trailing_md_error = GRPC_ERROR_NONE;
    stream->SetOnInitialMetadataCb(&recv_initial_md_cb, &initial_md_recv);
    stream->SetOnMsgCb(&recv_msg_cb, &msg_ptr);
    stream->SetOnCloseCb(&recv_trailing_md_cb, &trailing_md_recv);
    this->stream = stream;
  }

  StreamWrapper(ClientTransport* transport) :
    StreamWrapper(grpc_core::New<Stream>(transport->Ref(), -1)) {}

  ~StreamWrapper() {
    grpc_metadata_batch_destroy(&initial_md_recv);
    grpc_metadata_batch_destroy(&trailing_md_recv);
    msg_ptr.reset();
    GRPC_ERROR_UNREF(recv_initial_md_error);
    GRPC_ERROR_UNREF(recv_msg_cb_error);
    GRPC_ERROR_UNREF(recv_trailing_md_error);
    grpc_core::Delete<Stream>(stream);
  }

  void ResetMessage() {
    msg_ptr.reset();
    msg_cb_called = false;
    GRPC_ERROR_UNREF(recv_msg_cb_error);
    GRPC_CLOSURE_INIT(&recv_msg_cb, on_message, this, grpc_schedule_on_exec_ctx);
    stream->SetOnMsgCb(&recv_msg_cb, &msg_ptr);
  }

  Stream* stream;

  grpc_metadata_batch initial_md_recv;
  grpc_core::OrphanablePtr<grpc_core::ByteStream> msg_ptr;
  grpc_metadata_batch trailing_md_recv;

  grpc_error* recv_initial_md_error;
  grpc_error* recv_msg_cb_error;
  grpc_error* recv_trailing_md_error;

  grpc_closure recv_initial_md_cb;
  grpc_closure recv_msg_cb;
  grpc_closure recv_trailing_md_cb;

  bool initial_md_cb_called;
  bool msg_cb_called;
  bool trailing_md_cb_called;
};

static void on_initial_md(void* arg, grpc_error* error) {
  StreamWrapper* wrapper = (StreamWrapper*) arg;
  wrapper->initial_md_cb_called = true;
  wrapper->recv_initial_md_error = GRPC_ERROR_REF(error);
}

static void on_message(void* arg, grpc_error* error) {
  StreamWrapper* wrapper = (StreamWrapper*) arg;
  wrapper->msg_cb_called = true;
  wrapper->recv_msg_cb_error = GRPC_ERROR_REF(error);
}

static void on_close(void* arg, grpc_error* error) {
  StreamWrapper* wrapper = (StreamWrapper*) arg;
  wrapper->trailing_md_cb_called = true;
  wrapper->recv_trailing_md_error = GRPC_ERROR_REF(error);
}


class TransportE2E {
 public:
  TransportE2E() {
    client = grpc_core::New<ClientTransport>();
    server = grpc_core::New<ServerTransport>();

    grpc_slice custom_key = grpc_slice_from_static_string("custom_key");
    grpc_slice custom_val = grpc_slice_from_static_string("custom_val");

    // Client Initial Metadata
    grpc_metadata_batch_init(&client_initial_md);
    grpc_slice path = grpc_slice_from_static_string("/path");
    grpc_slice method = grpc_slice_from_static_string("POST");
    grpc_slice authority = grpc_slice_from_static_string("auth");
    grpc_slice scheme = grpc_slice_from_static_string("scheme");
    client_initial_mdelem[0].md = grpc_mdelem_from_slices(GRPC_MDSTR_PATH, path);
    client_initial_mdelem[1].md = grpc_mdelem_from_slices(GRPC_MDSTR_METHOD, method);
    client_initial_mdelem[2].md = grpc_mdelem_from_slices(GRPC_MDSTR_AUTHORITY, authority);
    client_initial_mdelem[3].md = grpc_mdelem_from_slices(GRPC_MDSTR_SCHEME, scheme);
    client_initial_mdelem[4].md = grpc_mdelem_from_slices(custom_key, custom_val);
    GPR_ASSERT(grpc_metadata_batch_link_tail(&client_initial_md, &client_initial_mdelem[0]) == GRPC_ERROR_NONE);
    GPR_ASSERT(grpc_metadata_batch_link_tail(&client_initial_md, &client_initial_mdelem[1]) == GRPC_ERROR_NONE);
    GPR_ASSERT(grpc_metadata_batch_link_tail(&client_initial_md, &client_initial_mdelem[2]) == GRPC_ERROR_NONE);
    GPR_ASSERT(grpc_metadata_batch_link_tail(&client_initial_md, &client_initial_mdelem[3]) == GRPC_ERROR_NONE);
    GPR_ASSERT(grpc_metadata_batch_link_tail(&client_initial_md, &client_initial_mdelem[4]) == GRPC_ERROR_NONE);

    // Server Initial Metadata
    grpc_metadata_batch_init(&server_initial_md);
    grpc_slice status = grpc_slice_from_static_string("200");
    server_initial_mdelem[0].md = grpc_mdelem_from_slices(GRPC_MDSTR_STATUS, status);
    server_initial_mdelem[1].md = grpc_mdelem_from_slices(custom_key, custom_val);
    GPR_ASSERT(grpc_metadata_batch_link_tail(&server_initial_md, &server_initial_mdelem[0]) == GRPC_ERROR_NONE);
    GPR_ASSERT(grpc_metadata_batch_link_tail(&server_initial_md, &server_initial_mdelem[1]) == GRPC_ERROR_NONE);

    // Server Initial Metadata
    grpc_metadata_batch_init(&server_trailing_md);
    grpc_slice grpc_status = grpc_slice_from_static_string("0");
    server_trailing_mdelem[0].md = grpc_mdelem_from_slices(GRPC_MDSTR_GRPC_STATUS, grpc_status);
    server_trailing_mdelem[1].md = grpc_mdelem_from_slices(custom_key, custom_val);
    GPR_ASSERT(grpc_metadata_batch_link_tail(&server_trailing_md, &server_trailing_mdelem[0]) == GRPC_ERROR_NONE);
    GPR_ASSERT(grpc_metadata_batch_link_tail(&server_trailing_md, &server_trailing_mdelem[1]) == GRPC_ERROR_NONE);
  }

  ~TransportE2E() {
    grpc_core::Delete<ClientTransport>(client);
    grpc_core::Delete<ServerTransport>(server);
    grpc_metadata_batch_destroy(&client_initial_md);
    grpc_metadata_batch_destroy(&server_initial_md);
    grpc_metadata_batch_destroy(&server_trailing_md);
  }

  void Flush() {
   grpc_slice_buffer client_buffer;
   grpc_slice_buffer_init(&client_buffer);
   grpc_slice_buffer server_buffer;
   grpc_slice_buffer_init(&server_buffer);

   do {
     client->Read(&server_buffer);
     server->Read(&client_buffer);
     client->Flush(&client_buffer);
     server->Flush(&server_buffer);
     grpc_core::ExecCtx::Get()->Flush();
   } while((client_buffer.length > 0 && server->Active()) || (server_buffer.length > 0  && client->Active()));


   grpc_slice_buffer_destroy(&client_buffer);
   grpc_slice_buffer_destroy(&server_buffer);
  }
  ClientTransport* client;
  ServerTransport* server;

  // Default metadata for tests
  grpc_metadata_batch client_initial_md;
  grpc_linked_mdelem client_initial_mdelem[5];
  grpc_metadata_batch server_initial_md;
  grpc_linked_mdelem server_initial_mdelem[2];
  grpc_metadata_batch server_trailing_md;
  grpc_linked_mdelem server_trailing_mdelem[2];
};

TEST(TransportE2E, UnaryRPC) {
 grpc_core::ExecCtx exec_ctx;

 TransportE2E e2e;
 e2e.Flush();

 StreamWrapper* client_stream = new StreamWrapper(e2e.client);
 client_stream->stream->SendInitialMetadata(&e2e.client_initial_md);
 e2e.Flush();

 StreamWrapper* server_stream = new StreamWrapper(e2e.server->new_streams);
 e2e.server->new_streams = e2e.server->new_streams->next;
 e2e.Flush();
 GPR_ASSERT(server_stream->initial_md_cb_called);
 GPR_ASSERT(md_is_eq(&server_stream->initial_md_recv, &e2e.client_initial_md));

 server_stream->stream->SendInitialMetadata(&e2e.server_initial_md);
 e2e.Flush();
 GPR_ASSERT(client_stream->initial_md_cb_called);
 GPR_ASSERT(md_is_eq(&client_stream->initial_md_recv, &e2e.server_initial_md));

 grpc_slice_buffer client_msg;
 grpc_slice_buffer_init(&client_msg);
 gpr_slice_buffer_add(&client_msg, grpc_slice_from_static_string("foo"));
 gpr_slice_buffer_add(&client_msg, grpc_slice_from_static_string("bar"));
 grpc_core::SliceBufferByteStream client_msg_stream(&client_msg, 0);
 grpc_core::OrphanablePtr<grpc_core::ByteStream> client_msg_ptr(&client_msg_stream);
 client_stream->stream->SendMessage(&client_msg_ptr);
 e2e.Flush();
 GPR_ASSERT(server_stream->msg_cb_called);
 GPR_ASSERT(msg_is_eq(&server_stream->msg_ptr, grpc_slice_from_static_string("foobar")));

 grpc_slice_buffer server_msg;
 grpc_slice_buffer_init(&server_msg);
 gpr_slice_buffer_add(&server_msg, grpc_slice_from_static_string("foo"));
 gpr_slice_buffer_add(&server_msg, grpc_slice_from_static_string("bar"));
 grpc_core::SliceBufferByteStream server_msg_stream(&server_msg, 0);
 grpc_core::OrphanablePtr<grpc_core::ByteStream> server_msg_ptr(&server_msg_stream);
 server_stream->stream->SendMessage(&server_msg_ptr);
 e2e.Flush();
 GPR_ASSERT(client_stream->msg_cb_called);
 GPR_ASSERT(msg_is_eq(&client_stream->msg_ptr, grpc_slice_from_static_string("foobar")));

 client_stream->stream->Close();
 e2e.Flush();
 GPR_ASSERT(server_stream->trailing_md_cb_called);

 server_stream->stream->SendTrailers(&e2e.server_trailing_md);
 e2e.Flush();
 GPR_ASSERT(client_stream->trailing_md_cb_called);
 GPR_ASSERT(md_is_eq(&client_stream->trailing_md_recv, &e2e.server_trailing_md));

 delete client_stream;
 delete server_stream;
}

TEST(TransportE2E, BatchedUnaryRPC) {
 grpc_core::ExecCtx exec_ctx;

 TransportE2E e2e;
 e2e.Flush();

 StreamWrapper* client_stream = new StreamWrapper(e2e.client);
 client_stream->stream->SendInitialMetadata(&e2e.client_initial_md);
 grpc_slice_buffer client_msg;
 grpc_slice_buffer_init(&client_msg);
 gpr_slice_buffer_add(&client_msg, grpc_slice_from_static_string("foo"));
 gpr_slice_buffer_add(&client_msg, grpc_slice_from_static_string("bar"));
 grpc_core::SliceBufferByteStream client_msg_stream(&client_msg, 0);
 grpc_core::OrphanablePtr<grpc_core::ByteStream> client_msg_ptr(&client_msg_stream);
 client_stream->stream->SendMessage(&client_msg_ptr);
 client_stream->stream->Close();
 e2e.Flush();

 StreamWrapper* server_stream = new StreamWrapper(e2e.server->new_streams);
 e2e.server->new_streams = e2e.server->new_streams->next;
 e2e.Flush();
 GPR_ASSERT(server_stream->initial_md_cb_called);
 GPR_ASSERT(md_is_eq(&server_stream->initial_md_recv, &e2e.client_initial_md));
 GPR_ASSERT(server_stream->msg_cb_called);
 GPR_ASSERT(msg_is_eq(&server_stream->msg_ptr, grpc_slice_from_static_string("foobar")));
 GPR_ASSERT(server_stream->trailing_md_cb_called);

 server_stream->stream->SendInitialMetadata(&e2e.server_initial_md);
 grpc_slice_buffer server_msg;
 grpc_slice_buffer_init(&server_msg);
 gpr_slice_buffer_add(&server_msg, grpc_slice_from_static_string("foo"));
 gpr_slice_buffer_add(&server_msg, grpc_slice_from_static_string("bar"));
 grpc_core::SliceBufferByteStream server_msg_stream(&server_msg, 0);
 grpc_core::OrphanablePtr<grpc_core::ByteStream> server_msg_ptr(&server_msg_stream);
 server_stream->stream->SendMessage(&server_msg_ptr);
 server_stream->stream->SendTrailers(&e2e.server_trailing_md);
 e2e.Flush();

 GPR_ASSERT(client_stream->initial_md_cb_called);
 GPR_ASSERT(md_is_eq(&client_stream->initial_md_recv, &e2e.server_initial_md));
 GPR_ASSERT(client_stream->msg_cb_called);
 GPR_ASSERT(msg_is_eq(&client_stream->msg_ptr, grpc_slice_from_static_string("foobar")));
 GPR_ASSERT(client_stream->trailing_md_cb_called);
 GPR_ASSERT(md_is_eq(&client_stream->trailing_md_recv, &e2e.server_trailing_md));

 delete client_stream;
 delete server_stream;
}


TEST(TransportE2E, ServerGoaway) {
 grpc_core::ExecCtx exec_ctx;

 TransportE2E e2e;
 e2e.Flush();

 // Send a GOAWAY to the client
 e2e.server->Shutdown(GRPC_ERROR_NONE);

 StreamWrapper* client_stream = new StreamWrapper(e2e.client);
 client_stream->stream->SendInitialMetadata(&e2e.client_initial_md);
 e2e.Flush();

 GPR_ASSERT(e2e.server->new_streams == nullptr);
 delete client_stream;
}

TEST(TransportE2E, ClientCancel) {
 grpc_core::ExecCtx exec_ctx;

 TransportE2E e2e;
 e2e.Flush();

 StreamWrapper* client_stream = new StreamWrapper(e2e.client);
 client_stream->stream->SendInitialMetadata(&e2e.client_initial_md);
 e2e.Flush();

 StreamWrapper* server_stream = new StreamWrapper(e2e.server->new_streams);
 e2e.server->new_streams = e2e.server->new_streams->next;
 e2e.Flush();
 GPR_ASSERT(server_stream->initial_md_cb_called);
 GPR_ASSERT(md_is_eq(&server_stream->initial_md_recv, &e2e.client_initial_md));

 client_stream->stream->Shutdown(GRPC_ERROR_CANCELLED);
 e2e.Flush();
 GPR_ASSERT(client_stream->initial_md_cb_called);
 GPR_ASSERT(server_stream->msg_cb_called);
 GPR_ASSERT(client_stream->msg_cb_called);
 GPR_ASSERT(server_stream->trailing_md_cb_called);
 GPR_ASSERT(client_stream->trailing_md_cb_called);

 delete client_stream;
 delete server_stream;
}

TEST(TransportE2E, ServerCancel) {
 grpc_core::ExecCtx exec_ctx;

 TransportE2E e2e;
 e2e.Flush();

 StreamWrapper* client_stream = new StreamWrapper(e2e.client);
 client_stream->stream->SendInitialMetadata(&e2e.client_initial_md);
 e2e.Flush();

 StreamWrapper* server_stream = new StreamWrapper(e2e.server->new_streams);
 e2e.server->new_streams = e2e.server->new_streams->next;
 e2e.Flush();
 GPR_ASSERT(server_stream->initial_md_cb_called);
 GPR_ASSERT(md_is_eq(&server_stream->initial_md_recv, &e2e.client_initial_md));

 server_stream->stream->Shutdown(GRPC_ERROR_CANCELLED);
 e2e.Flush();
 GPR_ASSERT(client_stream->initial_md_cb_called);
 GPR_ASSERT(server_stream->msg_cb_called);
 GPR_ASSERT(client_stream->msg_cb_called);
 GPR_ASSERT(server_stream->trailing_md_cb_called);
 GPR_ASSERT(client_stream->trailing_md_cb_called);

 delete client_stream;
 delete server_stream;
}

TEST(TransportE2E, LargeMessage) {
 grpc_core::ExecCtx exec_ctx;

 TransportE2E e2e;
 e2e.Flush();

 StreamWrapper* client_stream = new StreamWrapper(e2e.client);
 client_stream->stream->SendInitialMetadata(&e2e.client_initial_md);
 e2e.Flush();

 StreamWrapper* server_stream = new StreamWrapper(e2e.server->new_streams);
 e2e.server->new_streams = e2e.server->new_streams->next;
 e2e.Flush();
 GPR_ASSERT(server_stream->initial_md_cb_called);
 GPR_ASSERT(md_is_eq(&server_stream->initial_md_recv, &e2e.client_initial_md));

 server_stream->stream->SendInitialMetadata(&e2e.server_initial_md);
 e2e.Flush();
 GPR_ASSERT(client_stream->initial_md_cb_called);
 GPR_ASSERT(md_is_eq(&client_stream->initial_md_recv, &e2e.server_initial_md));

 grpc_slice_buffer client_msg;
 grpc_slice_buffer_init(&client_msg);
 char* big_string = (char*) gpr_malloc(sizeof(char) * (1 << 17));
 for (int i = 0; i < 1<< 17; i ++) {
   big_string[i] = 'A' + (random() % 26);
 }
 gpr_slice big_string_slice = grpc_slice_from_copied_buffer(big_string, 1 << 17);
 gpr_slice_buffer_add(&client_msg, gpr_slice_ref(big_string_slice));
 grpc_core::SliceBufferByteStream client_msg_stream(&client_msg, 0);
 grpc_core::OrphanablePtr<grpc_core::ByteStream> client_msg_ptr(&client_msg_stream);
 client_stream->stream->SendMessage(&client_msg_ptr);
 e2e.Flush();
 GPR_ASSERT(server_stream->msg_cb_called);
 GPR_ASSERT(msg_is_eq(&server_stream->msg_ptr, big_string_slice));

 grpc_slice_buffer server_msg;
 grpc_slice_buffer_init(&server_msg);
 gpr_slice_buffer_add(&server_msg, gpr_slice_ref(big_string_slice));
 grpc_core::SliceBufferByteStream server_msg_stream(&server_msg, 0);
 grpc_core::OrphanablePtr<grpc_core::ByteStream> server_msg_ptr(&server_msg_stream);
 server_stream->stream->SendMessage(&server_msg_ptr);
 e2e.Flush();
 GPR_ASSERT(client_stream->msg_cb_called);
 GPR_ASSERT(msg_is_eq(&client_stream->msg_ptr, big_string_slice));

 client_stream->stream->Close();
 e2e.Flush();
 GPR_ASSERT(server_stream->trailing_md_cb_called);

 server_stream->stream->SendTrailers(&e2e.server_trailing_md);
 e2e.Flush();
 GPR_ASSERT(client_stream->trailing_md_cb_called);
 GPR_ASSERT(md_is_eq(&client_stream->trailing_md_recv, &e2e.server_trailing_md));

 gpr_slice_unref(big_string_slice);
 gpr_free(big_string);
 delete client_stream;
 delete server_stream;
}

TEST(TransportE2E, ClientStreamingRPC) {
 grpc_core::ExecCtx exec_ctx;

 TransportE2E e2e;
 e2e.Flush();

 StreamWrapper* client_stream = new StreamWrapper(e2e.client);
 client_stream->stream->SendInitialMetadata(&e2e.client_initial_md);
 e2e.Flush();

 StreamWrapper* server_stream = new StreamWrapper(e2e.server->new_streams);
 e2e.server->new_streams = e2e.server->new_streams->next;
 e2e.Flush();
 GPR_ASSERT(server_stream->initial_md_cb_called);
 GPR_ASSERT(md_is_eq(&server_stream->initial_md_recv, &e2e.client_initial_md));

 server_stream->stream->SendInitialMetadata(&e2e.server_initial_md);
 e2e.Flush();
 GPR_ASSERT(client_stream->initial_md_cb_called);
 GPR_ASSERT(md_is_eq(&client_stream->initial_md_recv, &e2e.server_initial_md));

 for (size_t i = 0; i < 100; i++) {
   grpc_slice_buffer client_msg;
   grpc_slice_buffer_init(&client_msg);
   gpr_slice_buffer_add(&client_msg, grpc_slice_from_static_string("foo"));
   gpr_slice_buffer_add(&client_msg, grpc_slice_from_static_string("bar"));
   grpc_core::SliceBufferByteStream client_msg_stream(&client_msg, 0);
   grpc_core::OrphanablePtr<grpc_core::ByteStream> client_msg_ptr(&client_msg_stream);
   client_stream->stream->SendMessage(&client_msg_ptr);
   e2e.Flush();
 }

 for (size_t i = 0; i < 100; i++) {
   GPR_ASSERT(server_stream->msg_cb_called);
   GPR_ASSERT(msg_is_eq(&server_stream->msg_ptr, grpc_slice_from_static_string("foobar")));
   server_stream->ResetMessage();
   e2e.Flush();
 }

  grpc_slice_buffer server_msg;
  grpc_slice_buffer_init(&server_msg);
  gpr_slice_buffer_add(&server_msg, grpc_slice_from_static_string("foo"));
  gpr_slice_buffer_add(&server_msg, grpc_slice_from_static_string("bar"));
  grpc_core::SliceBufferByteStream server_msg_stream(&server_msg, 0);
  grpc_core::OrphanablePtr<grpc_core::ByteStream> server_msg_ptr(&server_msg_stream);
  server_stream->stream->SendMessage(&server_msg_ptr);
  e2e.Flush();
  GPR_ASSERT(client_stream->msg_cb_called);
  GPR_ASSERT(msg_is_eq(&client_stream->msg_ptr, grpc_slice_from_static_string("foobar")));

  client_stream->stream->Close();
  e2e.Flush();
  GPR_ASSERT(server_stream->trailing_md_cb_called);

 server_stream->stream->SendTrailers(&e2e.server_trailing_md);
 e2e.Flush();
 GPR_ASSERT(client_stream->trailing_md_cb_called);
 GPR_ASSERT(md_is_eq(&client_stream->trailing_md_recv, &e2e.server_trailing_md));

 delete client_stream;
 delete server_stream;
}

TEST(TransportE2E, ServerStreamingRPC) {
 grpc_core::ExecCtx exec_ctx;

 TransportE2E e2e;
 e2e.Flush();

 StreamWrapper* client_stream = new StreamWrapper(e2e.client);
 client_stream->stream->SendInitialMetadata(&e2e.client_initial_md);
 e2e.Flush();

 StreamWrapper* server_stream = new StreamWrapper(e2e.server->new_streams);
 e2e.server->new_streams = e2e.server->new_streams->next;
 e2e.Flush();
 GPR_ASSERT(server_stream->initial_md_cb_called);
 GPR_ASSERT(md_is_eq(&server_stream->initial_md_recv, &e2e.client_initial_md));

 server_stream->stream->SendInitialMetadata(&e2e.server_initial_md);
 e2e.Flush();
 GPR_ASSERT(client_stream->initial_md_cb_called);
 GPR_ASSERT(md_is_eq(&client_stream->initial_md_recv, &e2e.server_initial_md));

 grpc_slice_buffer client_msg;
 grpc_slice_buffer_init(&client_msg);
 gpr_slice_buffer_add(&client_msg, grpc_slice_from_static_string("foo"));
 gpr_slice_buffer_add(&client_msg, grpc_slice_from_static_string("bar"));
 grpc_core::SliceBufferByteStream client_msg_stream(&client_msg, 0);
 grpc_core::OrphanablePtr<grpc_core::ByteStream> client_msg_ptr(&client_msg_stream);
 client_stream->stream->SendMessage(&client_msg_ptr);
 e2e.Flush();
 GPR_ASSERT(server_stream->msg_cb_called);
 GPR_ASSERT(msg_is_eq(&server_stream->msg_ptr, grpc_slice_from_static_string("foobar")));

 for (size_t i = 0; i < 100; i++) {
   grpc_slice_buffer server_msg;
   grpc_slice_buffer_init(&server_msg);
   gpr_slice_buffer_add(&server_msg, grpc_slice_from_static_string("foo"));
   gpr_slice_buffer_add(&server_msg, grpc_slice_from_static_string("bar"));
   grpc_core::SliceBufferByteStream server_msg_stream(&server_msg, 0);
   grpc_core::OrphanablePtr<grpc_core::ByteStream> server_msg_ptr(&server_msg_stream);
   server_stream->stream->SendMessage(&server_msg_ptr);
   e2e.Flush();
 }

 for (size_t i = 0; i < 100; i++) {
   GPR_ASSERT(client_stream->msg_cb_called);
   GPR_ASSERT(msg_is_eq(&client_stream->msg_ptr, grpc_slice_from_static_string("foobar")));
   client_stream->ResetMessage();
   e2e.Flush();
 }

  client_stream->stream->Close();
  e2e.Flush();
  GPR_ASSERT(server_stream->trailing_md_cb_called);

 server_stream->stream->SendTrailers(&e2e.server_trailing_md);
 e2e.Flush();
 GPR_ASSERT(client_stream->trailing_md_cb_called);
 GPR_ASSERT(md_is_eq(&client_stream->trailing_md_recv, &e2e.server_trailing_md));

 delete client_stream;
 delete server_stream;
}

TEST(TransportE2E, PingPongStreamingRPC) {
 grpc_core::ExecCtx exec_ctx;

 TransportE2E e2e;
 e2e.Flush();

 StreamWrapper* client_stream = new StreamWrapper(e2e.client);
 client_stream->stream->SendInitialMetadata(&e2e.client_initial_md);
 e2e.Flush();

 StreamWrapper* server_stream = new StreamWrapper(e2e.server->new_streams);
 e2e.server->new_streams = e2e.server->new_streams->next;
 e2e.Flush();
 GPR_ASSERT(server_stream->initial_md_cb_called);
 GPR_ASSERT(md_is_eq(&server_stream->initial_md_recv, &e2e.client_initial_md));

 server_stream->stream->SendInitialMetadata(&e2e.server_initial_md);
 e2e.Flush();
 GPR_ASSERT(client_stream->initial_md_cb_called);
 GPR_ASSERT(md_is_eq(&client_stream->initial_md_recv, &e2e.server_initial_md));

 for (size_t i = 0; i < 100; i++) {
   grpc_slice_buffer client_msg;
   grpc_slice_buffer_init(&client_msg);
   gpr_slice_buffer_add(&client_msg, grpc_slice_from_static_string("foo"));
   gpr_slice_buffer_add(&client_msg, grpc_slice_from_static_string("bar"));
   grpc_core::SliceBufferByteStream client_msg_stream(&client_msg, 0);
   grpc_core::OrphanablePtr<grpc_core::ByteStream> client_msg_ptr(&client_msg_stream);
   client_stream->stream->SendMessage(&client_msg_ptr);
   e2e.Flush();
   GPR_ASSERT(server_stream->msg_cb_called);
   GPR_ASSERT(msg_is_eq(&server_stream->msg_ptr, grpc_slice_from_static_string("foobar")));
   server_stream->ResetMessage();

   grpc_slice_buffer server_msg;
   grpc_slice_buffer_init(&server_msg);
   gpr_slice_buffer_add(&server_msg, grpc_slice_from_static_string("foo"));
   gpr_slice_buffer_add(&server_msg, grpc_slice_from_static_string("bar"));
   grpc_core::SliceBufferByteStream server_msg_stream(&server_msg, 0);
   grpc_core::OrphanablePtr<grpc_core::ByteStream> server_msg_ptr(&server_msg_stream);
   server_stream->stream->SendMessage(&server_msg_ptr);
   e2e.Flush();
   GPR_ASSERT(client_stream->msg_cb_called);
   GPR_ASSERT(msg_is_eq(&client_stream->msg_ptr, grpc_slice_from_static_string("foobar")));
   client_stream->ResetMessage();
 }

 client_stream->stream->Close();
 e2e.Flush();
 GPR_ASSERT(server_stream->trailing_md_cb_called);

 server_stream->stream->SendTrailers(&e2e.server_trailing_md);
 e2e.Flush();
 GPR_ASSERT(client_stream->trailing_md_cb_called);

 server_stream->stream->SendTrailers(&e2e.server_trailing_md);
 e2e.Flush();
 GPR_ASSERT(client_stream->trailing_md_cb_called);
 GPR_ASSERT(md_is_eq(&client_stream->trailing_md_recv, &e2e.server_trailing_md));

 delete client_stream;
 delete server_stream;
}

} //namespace
} //namespace nghttp2
}  // namespace grpc_core

int main(int argc, char** argv) {
  grpc_init();
  grpc_test_init(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  int retval = RUN_ALL_TESTS();
  grpc_shutdown();
  return retval;
}
