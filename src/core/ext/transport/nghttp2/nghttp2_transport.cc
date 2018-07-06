#include "src/core/ext/transport/nghttp2/nghttp2_transport.h"

#include <string.h>
#include <grpc/slice.h>

#include "src/core/lib/gpr/useful.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/slice/slice_string_helpers.h"

#define EMPTY_FRAME (grpc_core::OrphanablePtr<grpc_core::ByteStream>*)0x1

namespace grpc_core {
namespace nghttp2 {
Transport::Transport() {
  streams_ = nullptr;
  error_ = GRPC_ERROR_NONE;
  out_ = nullptr;
  gpr_mu_init(&mutex_);
}

Transport::~Transport() {
  nghttp2_session_del(session_);
  GPR_ASSERT(streams_ == nullptr);
  GPR_ASSERT(out_ == nullptr);
  GRPC_ERROR_UNREF(error_);
  gpr_mu_destroy(&mutex_);
}

void Transport::Read(grpc_slice_buffer* in) {
  while (nghttp2_session_want_read(session_) && in->length > 0) {
    in_ = grpc_slice_buffer_take_first(in);
    ssize_t ret = nghttp2_session_mem_recv(session_, GRPC_SLICE_START_PTR(in_),
                                           GRPC_SLICE_LENGTH(in_));
    //TODO error handling
    GPR_ASSERT(ret >=0);
    if ((size_t)ret < GRPC_SLICE_LENGTH(in_)) {
      grpc_slice_buffer_undo_take_first(in, in_);
      grpc_slice_buffer garbage;
      grpc_slice_buffer_init(&garbage);
      grpc_slice_buffer_move_first(in, ret, &garbage);
      grpc_slice_buffer_destroy(&garbage);
    } else {
      grpc_slice_unref_internal(in_);
    }
  }
}

void Transport::Flush(grpc_slice_buffer* out) {
  out_ = out;
  while (nghttp2_session_want_write(session_)) {
    const uint8_t* data;
    ssize_t len = nghttp2_session_mem_send(session_, &data);
    //TODO error handling
    GPR_ASSERT(len >= 0);
      grpc_slice slice = grpc_slice_from_copied_buffer(
          reinterpret_cast<const char*>(data), len);
      grpc_slice_buffer_add(out_, slice);
  }
  out_ = nullptr;
}

bool Transport::Active() {
  return nghttp2_session_want_write(session_) || nghttp2_session_want_read(session_);
}

void Transport::Shutdown(grpc_error* error) {
  while (streams_) {
    streams_->Shutdown(GRPC_ERROR_REF(error));
    streams_ = streams_->next;
  }
  error_ = error;
  nghttp2_session_terminate_session(session_, NGHTTP2_NO_ERROR);
}

void Transport::Destroy() {
  session_ = nullptr;
}

void Transport::Lock() { gpr_mu_lock(&mutex_); }

void Transport::Unlock() { gpr_mu_unlock(&mutex_); }

ssize_t Transport::OnSendData(nghttp2_session* session, int32_t stream_id,
                              uint8_t* buf, size_t length, uint32_t* data_flags,
                              nghttp2_data_source* source, void* user_data) {
  Transport* transport = (Transport*)user_data;
  Stream* stream = (Stream*)nghttp2_session_get_stream_user_data(
      transport->session_, stream_id);
  if (!stream) {
    return 0;
  }
  if (stream->send_closed_ && stream->pending_msg_ == EMPTY_FRAME) {
    *data_flags = NGHTTP2_DATA_FLAG_EOF;
    stream->pending_msg_ = nullptr;
    return 0;
  }
  size_t remaining = (*stream->pending_msg_)->length() - stream->msg_bytes_sent + GRPC_SLICE_LENGTH(stream->pending_slice);
  if (!stream->msg_header_sent) {
    remaining += 5;
  }
  *data_flags =  NGHTTP2_DATA_FLAG_NO_COPY;
  if (remaining <= length) {
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    if (!stream->send_closed_) {
      *data_flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
    }
  }
  return GPR_MIN(length, remaining);
}

#define FRAMEHDR_LEN 9
int Transport::DataCb(nghttp2_session* session, nghttp2_frame* frame,
                      const uint8_t* framehd, size_t length,
                      nghttp2_data_source* source, void* user_data) {
  Transport* transport = (Transport*)user_data;
  Stream* stream = (Stream*)source->ptr;
  grpc_slice hdr = grpc_slice_from_copied_buffer(
      reinterpret_cast<const char*>(framehd), FRAMEHDR_LEN);
  grpc_slice_buffer_add(transport->out_, hdr);
  if (frame->data.padlen > 0) {
    uint8_t* pad = grpc_slice_buffer_tiny_add(transport->out_, 1);
    *pad = frame->data.padlen;
  }
  // Grpc Message length prefix
  if (!stream->msg_header_sent) {
    uint8_t* hdr = grpc_slice_buffer_tiny_add(transport->out_, 5);
    *hdr = 0;
    hdr++;
    uint32_t pre_len = (uint32_t) (*stream->pending_msg_)->length();
    *(uint32_t*)hdr = ((pre_len>>24)&0xff) |
           ((pre_len<<8)&0xff0000) |
           ((pre_len>>8)&0xff00) |
           ((pre_len<<24)&0xff000000);
    stream->msg_header_sent = 1;
    length -= 5;
  }
  if (length == 0) {
    // TODO This needs to be handled much cleaner
    if ((*stream->pending_msg_)->length() == 0) {
         stream->msg_bytes_sent = 0;
         stream->msg_header_sent = false;
         (*stream->pending_msg_).reset();
         stream->pending_msg_ = nullptr;
     }

    return 0;
  }
  if (GRPC_SLICE_LENGTH(stream->pending_slice) > 0) {
    if (GRPC_SLICE_LENGTH(stream->pending_slice) <= length) {
       grpc_slice_buffer_add(transport->out_, stream->pending_slice);
       length -= GRPC_SLICE_LENGTH(stream->pending_slice);
       stream->pending_slice = grpc_empty_slice();
    } else {
      grpc_slice_buffer_add(transport->out_, grpc_slice_sub(stream->pending_slice, 0, length));
      stream->pending_slice = grpc_slice_sub_no_ref(stream->pending_slice, length, GRPC_SLICE_LENGTH(stream->pending_slice));
      return 0;
    }
  }
  size_t remaining = (*stream->pending_msg_)->length() - stream->msg_bytes_sent;
  grpc_closure unused;
  while (remaining > 0) {
    grpc_slice slice;
    GPR_ASSERT((*stream->pending_msg_)->Next(SIZE_MAX, &unused));
    grpc_error* error = (*stream->pending_msg_)->Pull(&slice);
    GPR_ASSERT(error == GRPC_ERROR_NONE);
    remaining -= GRPC_SLICE_LENGTH(slice);
    stream->msg_bytes_sent += GRPC_SLICE_LENGTH(slice);
    if (GRPC_SLICE_LENGTH(slice) <= length) {
      grpc_slice_buffer_add(transport->out_, slice);
      length -= GRPC_SLICE_LENGTH(slice);
    } else {
      grpc_slice_buffer_add(transport->out_, grpc_slice_sub(slice, 0, length));
      stream->pending_slice = grpc_slice_sub_no_ref(slice, length, GRPC_SLICE_LENGTH(slice));
      length = 0;
      return 0;
    }
  }
  if (frame->data.padlen > 1) {
    size_t len = frame->data.padlen - 1;
    char* pad_buffer = (char*)gpr_malloc(sizeof(char) * len);
    memset(pad_buffer, 0, len);
    grpc_slice pad_slice = grpc_slice_from_copied_buffer(pad_buffer, len);
    grpc_slice_buffer_add(transport->out_, pad_slice);
    gpr_free(pad_buffer);
  }
  stream->msg_bytes_sent = 0;
  stream->msg_header_sent = false;
  (*stream->pending_msg_).reset();
  stream->pending_msg_ = nullptr;
  return 0;
}

int Transport::OnSendFrameComplete(nghttp2_session* session,
                                   const nghttp2_frame* frame,
                                   void* user_data) {
  Transport* transport = (Transport*)user_data;
  Stream* s = (Stream*)nghttp2_session_get_stream_user_data(
      transport->session_, frame->hd.stream_id);
  if (!s) {
    return 0;
  }
  if ((frame->hd.type == NGHTTP2_DATA && s->pending_msg_ == nullptr)
      || frame->hd.type == NGHTTP2_HEADERS) {
    s->SendNextFrame();
  }
  return 0;
}

int Transport::OnRecvHeader(nghttp2_session* session,
                            const nghttp2_frame* frame, const uint8_t* name,
                            size_t namelen, const uint8_t* value,
                            size_t valuelen, uint8_t flags, void* user_data) {
  (void)flags;
  Transport* transport = (Transport*)user_data;
  Stream* stream = (Stream*)nghttp2_session_get_stream_user_data(
      transport->session_, frame->hd.stream_id);
  if (!stream) {
    return 0;
  }
  grpc_slice key = grpc_slice_intern(grpc_slice_from_copied_buffer(
      reinterpret_cast<const char*>(name), namelen));
  grpc_slice val = grpc_slice_from_copied_buffer(
      reinterpret_cast<const char*>(value), valuelen);
  if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
    stream->AppendTrailingMetadata(key, val);
  } else {
    stream->AppendInitialMetadata(key, val);
  }
  return 0;
}

int Transport::OnRecvFrameBegin(nghttp2_session *session, const nghttp2_frame_hd *hd, void *user_data) {
  Transport* transport = (Transport*)user_data;
  Stream* stream = (Stream*)nghttp2_session_get_stream_user_data(
      transport->session_, hd->stream_id);
  if (!stream) {
    return 0;
  }
  if(hd->type == NGHTTP2_DATA && hd->length > 0 && stream->curr_msg_ == nullptr) {
    stream->curr_msg_ = grpc_core::New<Stream::MsgBuffer>();
  }
  return 0;
}

int Transport::OnRecvData(nghttp2_session* session, uint8_t flags,
                          int32_t stream_id, const uint8_t* data, size_t len,
                          void* user_data) {
  Transport* transport = (Transport*)user_data;
  Stream* stream = (Stream*)nghttp2_session_get_stream_user_data(
      transport->session_, stream_id);
  if(!stream) {
    return 0;
  }
  if (stream->curr_msg_->length < 0) {
    size_t hdr_len = GPR_MIN(len, 5 - stream->msg_header_offset);
    memcpy(stream->msg_header + stream->msg_header_offset, data, hdr_len);
    stream->msg_header_offset += hdr_len;
    if (stream->msg_header_offset == 5) {
      uint32_t pre_len = *((uint32_t*)(stream->msg_header+1));
      stream->curr_msg_->length = ((pre_len>>24)&0xff) |
           ((pre_len<<8)&0xff0000) |
           ((pre_len>>8)&0xff00) |
           ((pre_len<<24)&0xff000000);
    }
    data += hdr_len;
    len -= hdr_len;
  }

  uint8_t* in_start = GRPC_SLICE_START_PTR(transport->in_);
  size_t in_len = GRPC_SLICE_LENGTH(transport->in_);
  if (in_start <= data && in_len - (data - in_start) >= len) {
    grpc_slice data_slice = grpc_slice_sub(transport->in_, data - in_start, len + (data - in_start));
    grpc_slice_buffer_add(&stream->curr_msg_->buffer, data_slice);
  } else {
    grpc_slice data_slice =
      grpc_slice_from_copied_buffer(reinterpret_cast<const char*>(data), len);
    grpc_slice_buffer_add(&stream->curr_msg_->buffer, data_slice);
  }
  if((int)stream->curr_msg_->buffer.length == stream->curr_msg_->length) {
      if (stream->msgs_tail_ != nullptr) {
        stream->msgs_tail_->next=stream->curr_msg_;
        stream->msgs_tail_ = stream->curr_msg_;
      } else {
        stream->msgs_head_ = stream->curr_msg_;
        stream->msgs_tail_ = stream->curr_msg_;
      }
      stream->curr_msg_ = nullptr;
      stream->msg_header_offset = 0;
    stream->OnMsg();
  }
  return 0;
}

int Transport::OnRecvFrameComplete(nghttp2_session* session,
                                   const nghttp2_frame* frame,
                                   void* user_data) {
  Transport* transport = (Transport*)user_data;
  Stream* stream = (Stream*)nghttp2_session_get_stream_user_data(
      transport->session_, frame->hd.stream_id);
  if (!stream) {
    // Stream is closed;
    return 0;
  }
  if (frame->hd.type == NGHTTP2_HEADERS &&
      !(frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
    stream->OnInitialMetadata();
  }
  if (frame->hd.type == NGHTTP2_RST_STREAM) {
    stream->error_ = GRPC_ERROR_CANCELLED;
    stream->Shutdown(GRPC_ERROR_CANCELLED);
  }
  if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
    stream->OnInitialMetadata();
    stream->OnClose();
  }
  return 0;
}

void Transport::RemoveStream(Stream* stream) {
  if (stream == streams_) {
    streams_ = streams_->next;
  }
  if (stream->prev != nullptr) {
    stream->prev->next = stream->next;
  }
  if (stream->next != nullptr) {
    stream->next->prev = stream->prev;
  }

  stream->next = nullptr;
  stream->prev = nullptr;
}

int Transport::OnClose(nghttp2_session* session, int32_t stream_id,
                       uint32_t error_code, void* user_data) {
  Transport* transport = (Transport*)user_data;
  Stream* stream = (Stream*)nghttp2_session_get_stream_user_data(
      transport->session_, stream_id);
  if (!stream) {
    return 0;
  }
  // grpc_error* error = ErrorFromCode(error_code);
  //stream->Shutdown(GRPC_ERROR_NONE);
  return 0;
}

int Transport::OnError(nghttp2_session *session, int lib_error_code, const char *msg,
                       size_t len, void *user_data) {
  return 0;
}

ClientTransport::ClientTransport() {
  nghttp2_session_callbacks* callbacks;
  nghttp2_session_callbacks_new(&callbacks);
  nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, OnHeaders);
  nghttp2_session_callbacks_set_on_header_callback(callbacks, OnRecvHeader);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
                                                            OnRecvData);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
                                                       OnRecvFrameComplete);
  nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, OnClose);
  nghttp2_session_callbacks_set_error_callback2(callbacks, OnError);
  nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, OnSendFrameComplete);
  nghttp2_session_callbacks_set_send_data_callback(callbacks, DataCb);
  nghttp2_session_callbacks_set_on_begin_frame_callback(callbacks, OnRecvFrameBegin);
  nghttp2_session_client_new(&session_, callbacks, this);
  nghttp2_session_callbacks_del(callbacks);

  nghttp2_settings_entry iv[1] = {
      {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};

  // TODO use return value here
  GPR_ASSERT(nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, 1) == 0);
}

int ClientTransport::OnHeaders(nghttp2_session* session,
                               const nghttp2_frame* frame, void* user_data) {
  ClientTransport* transport = (ClientTransport*)user_data;
  if (frame->hd.type != NGHTTP2_HEADERS) {
    // PUSH frames not allowed
    GPR_ASSERT(nghttp2_submit_rst_stream(transport->session_, NGHTTP2_FLAG_NONE,
                              frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR) == 0);
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  }
  return 0;
}

ServerTransport::ServerTransport() {
  nghttp2_session_callbacks* callbacks;
  nghttp2_session_callbacks_new(&callbacks);
  nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, OnHeaders);
  nghttp2_session_callbacks_set_on_header_callback(callbacks, OnRecvHeader);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
                                                            OnRecvData);
  nghttp2_session_callbacks_set_on_begin_frame_callback(callbacks, OnRecvFrameBegin);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
                                                       OnRecvFrameComplete);
  nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, OnClose);
  nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, OnSendFrameComplete);
  nghttp2_session_callbacks_set_send_data_callback(callbacks, DataCb);
  nghttp2_session_callbacks_set_error_callback2(callbacks, OnError);
  nghttp2_session_server_new(&session_, callbacks, this);
  nghttp2_session_callbacks_del(callbacks);
  new_streams = nullptr;
  nghttp2_settings_entry iv[2] = {
      {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
      // TODO increase this size
      {NGHTTP2_SETTINGS_MAX_FRAME_SIZE, 1 << 14}};
  // TODO handle return value
  GPR_ASSERT(nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, 2) == 0);
}

ServerTransport::~ServerTransport() {
 while(new_streams) {
   Stream* stream = new_streams;
   new_streams = new_streams->next;
   grpc_core::Delete<Stream>(stream);
 }
}

int ServerTransport::OnHeaders(nghttp2_session* session,
                               const nghttp2_frame* frame, void* user_data) {
  ServerTransport* transport = (ServerTransport*)user_data;
  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    // Reject PUSH headers and Trailers
    nghttp2_submit_rst_stream(transport->session_, NGHTTP2_FLAG_NONE,
                              frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  }
  Stream* stream =
      grpc_core::New<Stream>(transport->Ref(), frame->hd.stream_id);
  stream->next = transport->new_streams;
  transport->new_streams = stream;
  return 0;
};

Stream::Stream(RefCountedPtr<Transport> transport, ssize_t id) {
  id_ = id;
  grpc_metadata_batch_init(&initial_metadata_);
  grpc_metadata_batch_init(&trailing_metadata_);
  transport_ = transport;
  msgs_head_ = nullptr;
  msgs_tail_ = nullptr;
  curr_msg_ = nullptr;
  recv_closed_ = false;
  pending_frame_ = false;
  pending_msg_ = nullptr;
  pending_trailers_ = nullptr;
  send_closed_ = false;
  md_storage_offset_ = 0;
  error_ = GRPC_ERROR_NONE;
  msg_header_offset = 0;
  msg_header_sent = false;
  msg_bytes_sent = 0;
  pending_slice = grpc_empty_slice();

  initial_md_cb_ = nullptr;
  msg_cb_ = nullptr;
  close_cb_ = nullptr;
  if (id_ > 0) {
    GPR_ASSERT(nghttp2_session_set_stream_user_data(transport->session_, id_, this) == 0);
  }
}

Stream::~Stream() {
  grpc_metadata_batch_destroy(&initial_metadata_);
  grpc_metadata_batch_destroy(&trailing_metadata_);
}

void Stream::SendInitialMetadata(grpc_metadata_batch* batch) {
  nghttp2_nv nva[16];
  pending_frame_ = true;
  grpc_linked_mdelem* curr = batch->list.head;
  // TODO no-copy
  for (size_t i = 0; i < batch->list.count; i++) {
    nva[i].name = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(
        GRPC_SLICE_START_PTR(GRPC_MDKEY(curr->md))));
    nva[i].namelen = GRPC_SLICE_LENGTH(GRPC_MDKEY(curr->md));
    nva[i].value = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(
        GRPC_SLICE_START_PTR(GRPC_MDVALUE(curr->md))));
    nva[i].valuelen = GRPC_SLICE_LENGTH(GRPC_MDVALUE(curr->md));
    curr = curr->next;
  }
  // TODO somethingwith the return value
  int ret = nghttp2_submit_headers(transport_->session_, NGHTTP2_NV_FLAG_NONE, id_,
                         nullptr, nva, batch->list.count, this);
  GPR_ASSERT(ret >= 0);
  if (id_ < 0) {
    id_ = ret;
  }
}

void Stream::SendMessage(grpc_core::OrphanablePtr<grpc_core::ByteStream>* msg) {
  pending_msg_ = msg;
  if (pending_frame_) {
    return;
  }
  pending_frame_ = true;
  nghttp2_data_provider data_prd;
  data_prd.source.ptr = this;
  data_prd.read_callback = Transport::OnSendData;
  // TODO handle error
  GPR_ASSERT(nghttp2_submit_data(transport_->session_, NGHTTP2_FLAG_END_STREAM, id_, &data_prd) == 0);
}

void Stream::Close() {
  send_closed_ = true;
  // We don't have a pending data frame queued up so send an empty frame
  if(!pending_msg_) {
    SendMessage(EMPTY_FRAME);
  }
}

void Stream::SendTrailers(grpc_metadata_batch* batch) {
  nghttp2_nv nva[64];
  //send_closed_ = true;
  if (pending_frame_) {
    GPR_ASSERT(pending_trailers_ == nullptr);
    pending_trailers_ = batch;
    return;
  }
  pending_frame_ = true;
  grpc_linked_mdelem* curr = batch->list.head;
  for (size_t i = 0; i < batch->list.count; i++) {
    nva[i].name = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(
        GRPC_SLICE_START_PTR(GRPC_MDKEY(curr->md))));
    nva[i].namelen = GRPC_SLICE_LENGTH(GRPC_MDKEY(curr->md));
    nva[i].value = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(
        GRPC_SLICE_START_PTR(GRPC_MDVALUE(curr->md))));
    nva[i].valuelen = GRPC_SLICE_LENGTH(GRPC_MDVALUE(curr->md));
    curr = curr->next;
  }
  // TODO handle error
  GPR_ASSERT(nghttp2_submit_trailer(transport_->session_, id_, nva, batch->list.count) == 0);
}

void Stream::AppendInitialMetadata(grpc_slice key, grpc_slice val) {
  grpc_linked_mdelem* storage = &metadata_storage_[md_storage_offset_++];
  storage->md = grpc_mdelem_from_slices(key, val);
  GPR_ASSERT(grpc_metadata_batch_link_tail(&initial_metadata_, storage) ==
             GRPC_ERROR_NONE);
}

void Stream::AppendTrailingMetadata(grpc_slice key, grpc_slice val) {
  grpc_linked_mdelem* storage = &metadata_storage_[md_storage_offset_++];
  storage->md = grpc_mdelem_from_slices(key, val);
  GPR_ASSERT(grpc_metadata_batch_link_tail(&trailing_metadata_, storage) ==
             GRPC_ERROR_NONE);
}

void Stream::MaybeFinishInitialMd() {
  if (initial_md_cb_ != nullptr) {
    if (initial_md_recv_) {
      grpc_metadata_batch_move(&initial_metadata_, target_initial_md_);
    }
    if (initial_md_recv_ || error_ != GRPC_ERROR_NONE) {
      GRPC_CLOSURE_SCHED(initial_md_cb_, error_);
      initial_md_cb_ = nullptr;
    }
  }
}

void Stream::MaybeFinishMsg() {
  if (msg_cb_ != nullptr) {
    if (msgs_head_ != nullptr) {
      recv_stream_.Init(&(msgs_head_->buffer), 0);
      target_msg_->reset(recv_stream_.get());
      Stream::MsgBuffer* old_msg = msgs_head_;
      msgs_head_ = msgs_head_->next;
      grpc_core::Delete<Stream::MsgBuffer>(old_msg);
      if(msgs_head_ == nullptr) {
        msgs_tail_ = nullptr;
      }
      GRPC_CLOSURE_SCHED(msg_cb_, error_);
      msg_cb_ = nullptr;
    } else if (error_ != GRPC_ERROR_NONE) {
      target_msg_->reset();
      GRPC_CLOSURE_SCHED(msg_cb_, error_);
      msg_cb_ = nullptr;
    } else if (recv_closed_) {
      target_msg_->reset();
      GRPC_CLOSURE_SCHED(msg_cb_, GRPC_ERROR_NONE);
      msg_cb_ = nullptr;
    }
  }
}

void Stream::MaybeFinishOnClose() {
  if (close_cb_ != nullptr) {
    if (recv_closed_ && msgs_head_ == nullptr) {
      grpc_metadata_batch_move(&trailing_metadata_, target_trailing_md_);
    }
    if ((recv_closed_ && msgs_head_ == nullptr) || error_ != GRPC_ERROR_NONE) {
      if (msg_cb_ != nullptr) {
        target_msg_->reset(nullptr);
        GRPC_CLOSURE_SCHED(msg_cb_, error_);
        msg_cb_ = nullptr;
      }
      GRPC_CLOSURE_SCHED(close_cb_, error_);
      close_cb_ = nullptr;
    }
  }
}

void Stream::SetOnInitialMetadataCb(grpc_closure* cb, grpc_metadata_batch* md) {
  initial_md_cb_ = cb;
  target_initial_md_ = md;
  MaybeFinishInitialMd();
}

void Stream::SetOnMsgCb(grpc_closure* cb,
                        grpc_core::OrphanablePtr<grpc_core::ByteStream>* msg) {
  target_msg_ = msg;
  msg_cb_ = cb;
  MaybeFinishMsg();
}

void Stream::SetOnCloseCb(grpc_closure* cb, grpc_metadata_batch* md) {
  close_cb_ = cb;
  target_trailing_md_ = md;
  MaybeFinishOnClose();
}

void Stream::OnInitialMetadata() {
  initial_md_recv_ = true;
  MaybeFinishInitialMd();
}

void Stream::OnMsg() { MaybeFinishMsg(); }

void Stream::OnClose() {
  recv_closed_ = true;
  MaybeFinishMsg();
  MaybeFinishOnClose();
}

void Stream::Shutdown(grpc_error* error) {
  if (error_ == GRPC_ERROR_NONE) {
    // TODO use correct http2 error code
    GPR_ASSERT(nghttp2_submit_rst_stream(transport_->session_, NGHTTP2_FLAG_NONE, id_, NGHTTP2_CANCEL) == 0);
    error_ = GRPC_ERROR_CANCELLED;
  }
  MaybeFinishInitialMd();
  MaybeFinishMsg();
  MaybeFinishOnClose();
}

void Stream::SendNextFrame() {
  pending_frame_ = false;
  if (pending_msg_ != nullptr) {
    SendMessage(pending_msg_);
  } else if (pending_trailers_ != nullptr) {
    SendTrailers(pending_trailers_);
  }
}

}  // namespace nghttp2
}  // namespace grpc_core

struct grpc_nghttp2_stream {
  grpc_core::nghttp2::Stream* stream;
};

struct grpc_nghttp2_transport {
  grpc_transport base;
  gpr_mu mu;
  int refs;
  /* accept stream callback */
  void (*accept_stream)(void* user_data, grpc_transport* transport,
                        const void* server_data);
  void* accept_stream_user_data;
  bool is_client;
  grpc_connectivity_state_tracker state_tracker;
  grpc_endpoint* ep;
  grpc_core::nghttp2::Transport* transport;
  bool pending_write;
  grpc_slice_buffer read_buf;
  grpc_slice_buffer write_buf;
  grpc_slice_buffer pending_write_buf;
  grpc_closure on_read;
  grpc_closure on_write;
};

void do_write_locked(grpc_nghttp2_transport* t) {
  if (t->pending_write) {
    t->transport->Flush(&t->pending_write_buf);
  } else {
    t->transport->Flush(&t->write_buf);
    if (t->write_buf.length > 0) {
      t->pending_write = true;
      t->refs++;
      grpc_endpoint_write(t->ep, &t->write_buf, &t->on_write);
    }
  }
}

static int init_stream(grpc_transport* gt, grpc_stream* gs,
                       grpc_stream_refcount* refcount, const void* server_data,
                       gpr_arena* arena) {
  grpc_nghttp2_transport* transport =
      reinterpret_cast<grpc_nghttp2_transport*>(gt);
  gpr_mu_lock(&transport->mu);
  grpc_nghttp2_stream* stream = reinterpret_cast<grpc_nghttp2_stream*>(gs);
  if (server_data != nullptr) {
    stream->stream = const_cast<grpc_core::nghttp2::Stream*>(
        reinterpret_cast<const grpc_core::nghttp2::Stream*>(server_data));
  } else {
    stream->stream =
        grpc_core::New<grpc_core::nghttp2::Stream>(transport->transport->Ref(), -1);
  }
  do_write_locked(transport);
  gpr_mu_unlock(&transport->mu);
  return 0;
}

static void destroy_stream(grpc_transport* gt, grpc_stream* gs,
                           grpc_closure* then_schedule_closure) {
  grpc_nghttp2_transport* transport = reinterpret_cast<grpc_nghttp2_transport*>(gt);
  gpr_mu_lock(&transport->mu);
  grpc_nghttp2_stream* stream = reinterpret_cast<grpc_nghttp2_stream*>(gs);
  grpc_core::Delete<grpc_core::nghttp2::Stream>(stream->stream);
  GRPC_CLOSURE_SCHED(then_schedule_closure, GRPC_ERROR_NONE);
  gpr_mu_unlock(&transport->mu);
}

static void set_pollset(grpc_transport* gt, grpc_stream* gs,
                        grpc_pollset* pollset) {
  grpc_nghttp2_transport* transport =
      reinterpret_cast<grpc_nghttp2_transport*>(gt);
  grpc_endpoint_add_to_pollset(transport->ep, pollset);
}

static void set_pollset_set(grpc_transport* gt, grpc_stream* gs,
                            grpc_pollset_set* pollset_set) {
  grpc_nghttp2_transport* transport =
      reinterpret_cast<grpc_nghttp2_transport*>(gt);
  grpc_endpoint_add_to_pollset_set(transport->ep, pollset_set);
}

static void perform_stream_op(grpc_transport* gt, grpc_stream* gs,
                              grpc_transport_stream_op_batch* op) {
  grpc_nghttp2_transport* transport = reinterpret_cast<grpc_nghttp2_transport*>(gt);
  gpr_mu_lock(&transport->mu);
  grpc_core::nghttp2::Stream* stream =((grpc_nghttp2_stream*)gs)->stream;
  grpc_transport_stream_op_batch_payload* op_payload = op->payload;

  /*
  if (grpc_http_trace.enabled()) {
    char* str = grpc_transport_stream_op_batch_string(op);
    gpr_log(GPR_INFO, "perform_stream_op_locked: %s; on_complete = %p", str,
            op->on_complete);
    gpr_free(str);
    if (op->send_initial_metadata) {
      log_metadata(op_payload->send_initial_metadata.send_initial_metadata,
                   s->id, t->is_client, true);
    }
    if (op->send_trailing_metadata) {
      log_metadata(op_payload->send_trailing_metadata.send_trailing_metadata,
                   s->id, t->is_client, false);
    }
  }
  */

  //if (op->collect_stats) {
  //  gpr_log(GPR_DEBUG, "nghttp2 transport doesn't support collect_stats op");
  //}

  if (op->cancel_stream) {
    stream->Shutdown(GRPC_ERROR_REF(op_payload->cancel_stream.cancel_error));
  }

  if (op->send_initial_metadata) {
    // TODO set peer string
    // TODO respect initial metadata flags
    stream->SendInitialMetadata(
        op_payload->send_initial_metadata.send_initial_metadata);
  }

  if (op->send_message) {
    stream->SendMessage(&op_payload->send_message.send_message);
  }

  if (op->send_trailing_metadata) {
    if (transport->is_client) {
      stream->Close();
    } else {
    stream->SendTrailers(
        op_payload->send_trailing_metadata.send_trailing_metadata);
    }
  }

  if (op->recv_initial_metadata) {
    // TODO respect trailing_metadata_available
    stream->SetOnInitialMetadataCb(
        op_payload->recv_initial_metadata.recv_initial_metadata_ready,
        op_payload->recv_initial_metadata.recv_initial_metadata);
  }

  if (op->recv_message) {
    stream->SetOnMsgCb(op_payload->recv_message.recv_message_ready,
                       op_payload->recv_message.recv_message);
  }

  if (op->recv_trailing_metadata) {
    stream->SetOnCloseCb(
        op_payload->recv_trailing_metadata.recv_trailing_metadata_ready,
        op_payload->recv_trailing_metadata.recv_trailing_metadata);
  }
  do_write_locked(transport);
  gpr_mu_unlock(&transport->mu);
  GRPC_CLOSURE_SCHED(op->on_complete, GRPC_ERROR_NONE);
}

static void delete_transport(grpc_nghttp2_transport* transport) {
  transport->transport->Unref();
  grpc_connectivity_state_destroy(&transport->state_tracker);
  grpc_slice_buffer_destroy(&transport->read_buf);
  grpc_slice_buffer_destroy(&transport->write_buf);
  grpc_slice_buffer_destroy(&transport->pending_write_buf);
  grpc_endpoint_destroy(transport->ep);
  gpr_mu_destroy(&transport->mu);
  gpr_free(transport);
}

static void destroy_transport(grpc_transport* t) {
  grpc_nghttp2_transport* transport =
      reinterpret_cast<grpc_nghttp2_transport*>(t);
  gpr_mu_lock(&transport->mu);
  transport->refs--;
  grpc_endpoint_shutdown(transport->ep, GRPC_ERROR_CREATE_FROM_STATIC_STRING("Destroy"));
  int refs = transport->refs;
  gpr_mu_unlock(&transport->mu);
  if (refs == 0) {
    delete_transport(transport);
  }
}

static void perform_transport_op(grpc_transport* gt, grpc_transport_op* op) {
  grpc_nghttp2_transport* transport =
      reinterpret_cast<grpc_nghttp2_transport*>(gt);
  gpr_mu_lock(&transport->mu);

  if (op->goaway_error) {
    transport->transport->Shutdown(GRPC_ERROR_REF(op->goaway_error));
    do_write_locked(transport);
  }

  if (op->set_accept_stream) {
    transport->accept_stream = op->set_accept_stream_fn;
    transport->accept_stream_user_data = op->set_accept_stream_user_data;
  }

  if (op->bind_pollset) {
    grpc_endpoint_add_to_pollset(transport->ep, op->bind_pollset);
  }

  if (op->bind_pollset_set) {
    grpc_endpoint_add_to_pollset_set(transport->ep, op->bind_pollset_set);
  }

  if (op->send_ping.on_initiate != nullptr || op->send_ping.on_ack != nullptr) {
    // TODO send ping
    // send_ping_locked(t, op->send_ping.on_initiate, op->send_ping.on_ack);
    // grpc_chttp2_initiate_write(t,
    // GRPC_CHTTP2_INITIATE_WRITE_APPLICATION_PING);
  }

  if (op->on_connectivity_state_change != nullptr) {
    grpc_connectivity_state_notify_on_state_change(
        &transport->state_tracker, op->connectivity_state,
        op->on_connectivity_state_change);
  }

  if (op->disconnect_with_error != GRPC_ERROR_NONE) {
    transport->transport->Shutdown(GRPC_ERROR_REF(op->disconnect_with_error));
    do_write_locked(transport);
  }
  gpr_mu_unlock(&transport->mu);
  GRPC_CLOSURE_RUN(op->on_consumed, GRPC_ERROR_NONE);
}

static grpc_endpoint* get_endpoint(grpc_transport* t) {
  return (reinterpret_cast<grpc_nghttp2_transport*>(t))->ep;
}

static const grpc_transport_vtable vtable = {sizeof(grpc_nghttp2_stream),
                                             "nghttp",
                                             init_stream,
                                             set_pollset,
                                             set_pollset_set,
                                             perform_stream_op,
                                             perform_transport_op,
                                             destroy_stream,
                                             destroy_transport,
                                             get_endpoint};

void on_write(void* arg, grpc_error* error) {
  grpc_nghttp2_transport* t = (grpc_nghttp2_transport*) arg;
  gpr_mu_lock(&t->mu);
  t->refs--;
  grpc_core::nghttp2::Transport* transport = t->transport;
  t->pending_write = false;
  grpc_slice_buffer_reset_and_unref_internal(&t->write_buf);
  grpc_slice_buffer_move_into(&t->pending_write_buf, &t->write_buf);
  if (error != GRPC_ERROR_NONE) {
    transport->Shutdown(GRPC_ERROR_REF(error));
  }
  if (error == GRPC_ERROR_NONE && transport->Active()) {
    do_write_locked(t);
  } else {
    grpc_connectivity_state_set(&t->state_tracker, GRPC_CHANNEL_SHUTDOWN,  GRPC_ERROR_CREATE_FROM_STATIC_STRING("Inatcive transport"),
                           "close_transport");
  }
  int refs = t->refs;
  gpr_mu_unlock(&t->mu);
  if (refs == 0) {
    delete_transport(t);
  }
}

void on_read(void* arg, grpc_error* error) {
  grpc_nghttp2_transport* t = (grpc_nghttp2_transport*) arg;
  gpr_mu_lock(&t->mu);
  t->refs--;
  grpc_core::nghttp2::Transport* transport = t->transport;
  grpc_core::nghttp2::Stream* new_streams = nullptr;
  if (error != GRPC_ERROR_NONE) {
    transport->Shutdown(GRPC_ERROR_REF(error));
  } else {
    transport->Read(&t->read_buf);
    if (!t->is_client) {
      grpc_core::nghttp2::ServerTransport* s = (grpc_core::nghttp2::ServerTransport*) transport;
      new_streams = s->new_streams;
      s->new_streams = nullptr;
    }
  }
  if (transport->Active() && error == GRPC_ERROR_NONE) {
    do_write_locked(t);
    t->refs++;
    grpc_endpoint_read(t->ep, &t->read_buf, &t->on_read);
  } else {
    grpc_connectivity_state_set(&t->state_tracker, GRPC_CHANNEL_SHUTDOWN, GRPC_ERROR_CREATE_FROM_STATIC_STRING("Inatcive transport"),
                           "close_transport");
  }
  int refs = t->refs;
  gpr_mu_unlock(&t->mu);
  if (refs == 0) {
    delete_transport(t);
    return;
  }
  while(new_streams != nullptr) {
    grpc_core::nghttp2::Stream* stream = new_streams;
    t->accept_stream(t->accept_stream_user_data, &t->base, stream);
    new_streams = new_streams->next;
  }
}

void grpc_nghttp2_transport_start_reading(
    grpc_transport* transport, grpc_slice_buffer* read_buffer,
    grpc_closure* notify_on_receive_settings) {
  grpc_nghttp2_transport* t =
      reinterpret_cast<grpc_nghttp2_transport*>(transport);
  if (read_buffer != nullptr) {
    grpc_slice_buffer_move_into(read_buffer, &t->read_buf);
    gpr_free(read_buffer);
  }
  t->refs++;
  // TODO notify on recv settings
  //t->notify_on_receive_settings = notify_on_receive_settings;
  on_read((void*) t, GRPC_ERROR_NONE);
}

grpc_transport* grpc_create_nghttp2_transport(
    const grpc_channel_args* channel_args, grpc_endpoint* ep, bool is_client) {
  grpc_core::nghttp2::Transport* transport;
  if (is_client) {
    transport = grpc_core::New<grpc_core::nghttp2::ClientTransport>();
  } else {
    transport = grpc_core::New<grpc_core::nghttp2::ServerTransport>();
  }
  grpc_nghttp2_transport* t = (grpc_nghttp2_transport*) gpr_malloc(sizeof(grpc_nghttp2_transport));
  t->is_client = is_client;
  t->ep = ep;
  t->base.vtable = &vtable;
  t->transport = transport;
  grpc_connectivity_state_init(
      &t->state_tracker, GRPC_CHANNEL_READY,
      is_client ? "client_transport" : "server_transport");
  grpc_slice_buffer_init(&t->read_buf);
  grpc_slice_buffer_init(&t->write_buf);
  grpc_slice_buffer_init(&t->pending_write_buf);
  t->pending_write = false;
  t->refs = 1;
  gpr_mu_init(&t->mu);
  GRPC_CLOSURE_INIT(&t->on_read, on_read, t,
                    grpc_schedule_on_exec_ctx);
  GRPC_CLOSURE_INIT(&t->on_write, on_write, t,
                    grpc_schedule_on_exec_ctx);
  return &t->base;
}
