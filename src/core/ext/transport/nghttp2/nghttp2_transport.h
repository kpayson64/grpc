#include <nghttp2/nghttp2.h>
#include "grpc/impl/codegen/slice.h"
#include "src/core/lib/gprpp/orphanable.h"
#include "src/core/lib/gprpp/ref_counted.h"
#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/transport/byte_stream.h"
#include "src/core/lib/transport/connectivity_state.h"
#include "src/core/lib/transport/metadata_batch.h"
#include "src/core/lib/transport/transport.h"
#include "src/core/lib/transport/transport_impl.h"
#include "src/core/lib/gprpp/manual_constructor.h"

grpc_transport* grpc_create_nghttp2_transport(
    const grpc_channel_args* channel_args, grpc_endpoint* ep, bool is_client);
void grpc_nghttp2_transport_start_reading(
    grpc_transport* transport, grpc_slice_buffer* read_buffer,
    grpc_closure* notify_on_receive_settings) ;

namespace grpc_core {
namespace nghttp2 {

class Stream;

class Transport : public RefCounted<Transport> {
  friend class Stream;

 public:
  Transport();
  ~Transport();
  // Read @in from the wire and process the bytes
  void Read(grpc_slice_buffer* in);

  // Dump any pending bytes from this transport into @out
  void Flush(grpc_slice_buffer* out);

  // Gracefully shutdown the transport by sending a GOAWAY
  void Shutdown(grpc_error* error);

  // Destroys the underlying session.  Any further ops on the
  // transport will fail.
  void Destroy();

  bool Active();

  // Any action on this transport must be done under this lock
  void Lock();
  void Unlock();

  void RemoveStream(Stream* stream);

  GRPC_ABSTRACT_BASE_CLASS;

 protected:
  // This function just uses the NGHTTP2_DATA_FLAG_NO_COPY to indicate DataCB
  // should be invoked
  static ssize_t OnSendData(nghttp2_session* session, int32_t stream_id,
                            uint8_t* buf, size_t length, uint32_t* data_flags,
                            nghttp2_data_source* source, void* user_data);

  // Appends the DATA frame to the current write buffer, out_
  static int DataCb(nghttp2_session* session, nghttp2_frame* frame,
                    const uint8_t* framehd, size_t length,
                    nghttp2_data_source* source, void* user_data);

  // Invoked once a complete frame has been sent.
  static int OnSendFrameComplete(nghttp2_session* session,
                                 const nghttp2_frame* frame, void* user_data);

  // Invoked at the start of the RPC
  static int OnRecvFrameBegin(nghttp2_session *session, const nghttp2_frame_hd *hd, void *user_data);
  // Invoked for each header (both initial and trailing)
  static int OnRecvHeader(nghttp2_session* session, const nghttp2_frame* frame,
                          const uint8_t* name, size_t namelen,
                          const uint8_t* value, size_t valuelen, uint8_t flags,
                          void* user_data);

  // Invoked when partial data from a DATA frame is received.
  static int OnRecvData(nghttp2_session* session, uint8_t flags,
                        int32_t stream_id, const uint8_t* data, size_t len,
                        void* user_data);

  // Invoked when a full frame has been received
  static int OnRecvFrameComplete(nghttp2_session* session,
                                 const nghttp2_frame* frame, void* user_data);

  // Invoked when a stream is closed (GOAWAY/RST_STREAM)
  static int OnClose(nghttp2_session* session, int32_t stream_id,
                     uint32_t error_code, void* user_data);

  // Invoked on an nghttp2 library error
  static int OnError(nghttp2_session *session, int lib_error_code, const char *msg,
                     size_t len, void *user_data);

  nghttp2_session* session_;
  Stream* streams_;

 private:
  gpr_mu mutex_;
  grpc_error* error_;
  grpc_slice_buffer* out_;
  grpc_slice in_;
};

class ClientTransport : public Transport {
 public:
  ClientTransport();

 private:
  // Invoked at the start of a new headers frame
  static int OnHeaders(nghttp2_session* session, const nghttp2_frame* frame,
                       void* user_data);
};

class ServerTransport : public Transport {
 public:
  ServerTransport();
  ~ServerTransport();

   Stream* new_streams;
 private:
  // Invoked at the start of a new headers frame
  static int OnHeaders(nghttp2_session* session, const nghttp2_frame* frame,
                       void* user_data);
};

class Stream {
  friend class Transport;
  friend class ServerTransport;
  friend class ClientTransport;

  struct MsgBuffer {
    MsgBuffer() {
      grpc_slice_buffer_init(&buffer);
      next = nullptr;
      length = -1;
    }
    ~MsgBuffer() {
      grpc_slice_buffer_destroy(&buffer);
    }
    int length;
    grpc_slice_buffer buffer;
    MsgBuffer* next;
  };

 public:
  // Creates a new stream on @transport, with stream id @id.
  // If @id is 0, allocates a new stream id.
  Stream(RefCountedPtr<Transport> transport, ssize_t id);
  ~Stream();

  // Puts the metadata on the transport
  void SendInitialMetadata(grpc_metadata_batch* metadata);
  // Send Message
  void SendMessage(grpc_core::OrphanablePtr<grpc_core::ByteStream>* msg);
  // Send Trailers
  void SendTrailers(grpc_metadata_batch* batch);
  void Close();

  void Shutdown(grpc_error* error);

  // Set on initial metadata
  void SetOnInitialMetadataCb(grpc_closure* cb, grpc_metadata_batch* md);
  void SetOnMsgCb(grpc_closure* cb,
                  grpc_core::OrphanablePtr<grpc_core::ByteStream>* msg);
  void SetOnCloseCb(grpc_closure* cb, grpc_metadata_batch* md);

  void OnInitialMetadata();
  void OnMsg();
  void OnClose();

 Stream* next;
 protected:
  void AppendInitialMetadata(grpc_slice key, grpc_slice val);
  void AppendTrailingMetadata(grpc_slice key, grpc_slice val);

  // NGHTTP2 has a limitation of 1 outstanding frame per stream.
  // This kludge allows us to batch everything
  void SendNextFrame();

  Stream* prev;

 private:
  ssize_t id_;
  void MaybeFinishInitialMd();
  void MaybeFinishMsg();
  void MaybeFinishOnClose();

  // Error if we got closed
  grpc_error* error_;

  // Recv Data
  grpc_metadata_batch initial_metadata_;
  grpc_metadata_batch trailing_metadata_;
  bool recv_closed_;
  bool initial_md_recv_;
  uint8_t msg_header[5];
  size_t msg_header_offset;
  MsgBuffer* msgs_head_;
  MsgBuffer* msgs_tail_;
  MsgBuffer* curr_msg_;

  // Send Data
  bool pending_frame_;
  grpc_core::OrphanablePtr<grpc_core::ByteStream>* pending_msg_;
  bool msg_header_sent;
  size_t msg_bytes_sent;
  grpc_slice pending_slice;
  grpc_metadata_batch* pending_trailers_;
  bool send_closed_;

  grpc_linked_mdelem metadata_storage_[32];
  size_t md_storage_offset_;

  // Callbacks
  grpc_closure* initial_md_cb_;
  grpc_closure* msg_cb_;
  grpc_closure* close_cb_;
  grpc_metadata_batch* target_initial_md_;
  grpc_metadata_batch* target_trailing_md_;
  grpc_core::OrphanablePtr<grpc_core::ByteStream>* target_msg_;
  grpc_core::ManualConstructor<grpc_core::SliceBufferByteStream> recv_stream_;

  // The transport
  RefCountedPtr<Transport> transport_;
};
}  // namespace nghttp2
}  // namespace grpc_core
