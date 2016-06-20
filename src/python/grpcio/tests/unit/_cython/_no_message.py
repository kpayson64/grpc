# Copyright 2016, Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""Test making many calls and immediately cancelling most of them."""
"""
from grpc import _common
import unittest
import time

from grpc._cython import cygrpc


_INFINITE_FUTURE = cygrpc.Timespec(float('+inf'))
_EMPTY_FLAGS = 0
_EMPTY_METADATA = cygrpc.Metadata(())

class CancelManyCallsTest(unittest.TestCase):

  def testCancelThenCall(self):
    server_completion_queue = cygrpc.CompletionQueue()
    server = cygrpc.Server()
    server.register_completion_queue(server_completion_queue)
    port = server.add_http2_port('[::]:0')
    server.start()
    channel = cygrpc.Channel('localhost:{}'.format(port))

    client_completion_queue = cygrpc.CompletionQueue()
    client_call = channel.create_call(
      None, _EMPTY_FLAGS, client_completion_queue, b'/twinkies', None,
      _INFINITE_FUTURE)


    metadata_tag = 'client_send_metadata'
    operations = (
    cygrpc.operation_send_message(b'', _EMPTY_FLAGS),
    #cygrpc.operation_send_close_from_client(_EMPTY_FLAGS),
    cygrpc.operation_receive_status_on_client(_EMPTY_FLAGS)
    )
    client_call.start_batch(
      (cygrpc.operation_send_initial_metadata(_common.metadata(_EMPTY_METADATA), _EMPTY_FLAGS),), 
      metadata_tag)
    client_call.start_batch(cygrpc.Operations(operations), 'main-tag')
    client_call.start_batch((cygrpc.operation_receive_message(_EMPTY_FLAGS),), 'recv-msg-tag')
    server_call_cq = cygrpc.CompletionQueue()
    server.request_call(server_call_cq, server_completion_queue, None)
    client_completion_queue.poll()
    
    event = server_completion_queue.poll()
    server_call = event.operation_call
    operations = (
      cygrpc.operation_send_initial_metadata(_EMPTY_METADATA, _EMPTY_FLAGS),
      #cygrpc.operation_receive_close_on_server(_EMPTY_FLAGS),
      cygrpc.operation_send_status_from_server(
          _EMPTY_METADATA, cygrpc.StatusCode.ok,
          'Method not found!', _EMPTY_FLAGS),
    )
    server_call.start_batch(
      operations, lambda ignored_event: (rpc_state, (),))
    #server_call.cancel()
    #server_call.start_batch((cygrpc.operation_send_status_from_server(
    #      _EMPTY_METADATA, cygrpc.StatusCode.ok, '', _EMPTY_FLAGS),), None)
    server_call_cq.poll()
    print("SENDING")
    print("BEFORE")
    client_completion_queue.poll()
    for op in client_completion_queue.poll().batch_operations:
     if op.type == cygrpc.OperationType.receive_status_on_client:
      code = _common.CYGRPC_STATUS_CODE_TO_STATUS_CODE.get(
        op.received_status_code)
      print code
      print op.received_status_details
      #state.details = batch_operation.received_status_details
    

    #time.sleep(5)

    #client_call.cancel()
    
    #message_tag = 'client_send_message'
    #operations = (cygrpc.operation_send_message(b'\x45\x56', _EMPTY_FLAGS),)
    #client_call.start_batch(cygrpc.Operations(operations), message_tag)

    #event = client_completion_queue.poll()
    #self.assertEquals(event.tag, metadata_tag)
    #self.assertEquals(event.success, False)
    #event = client_completion_queue.poll()
    #self.assertEquals(event.tag, message_tag)
    #self.assertEquals(event.success, False)


if __name__ == '__main__':
  unittest.main(verbosity=2)
"""
