# Copyright 2017 gRPC authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cimport cpython
from libc cimport string
import sys
import errno
gevent_g = None
gevent_socket = None
gevent_core = None
gevent_hub = None
gevent_event = None

g_greenlets = set()

cdef grpc_socket_vtable gevent_socket_vtable
cdef grpc_custom_timer_vtable gevent_timer_vtable
cdef grpc_custom_poller_vtable gevent_pollset_vtable

def initialize_grpc_gevent_loop():
  # Lazily import gevent
  global gevent_socket
  global gevent_g
  global gevent_core
  global gevent_hub
  global gevent_event
  import gevent
  gevent_g = gevent
  import gevent.socket
  gevent_socket = gevent.socket
  import gevent.core
  gevent_core = gevent.core
  import gevent.hub
  gevent_hub = gevent.hub
  import gevent.event
  gevent_event = gevent.event

  gevent_socket_vtable.init = socket_init
  gevent_socket_vtable.connect = socket_connect
  gevent_socket_vtable.destroy = socket_destroy
  gevent_socket_vtable.shutdown = socket_shutdown
  gevent_socket_vtable.close = socket_close
  gevent_socket_vtable.write = socket_write
  gevent_socket_vtable.read = socket_read
  gevent_socket_vtable.getpeername = socket_getpeername
  gevent_socket_vtable.getsockname = socket_getsockname
  gevent_socket_vtable.setsockopt = socket_setsockopt
  gevent_socket_vtable.bind = socket_bind
  gevent_socket_vtable.listen = socket_listen
  gevent_socket_vtable.accept = socket_accept
  grpc_custom_endpoint_init(&gevent_socket_vtable)

  gevent_timer_vtable.start = timer_start
  gevent_timer_vtable.stop = timer_stop
  grpc_custom_timer_init(&gevent_timer_vtable)

  gevent_pollset_vtable.run_loop = run_loop
  gevent_pollset_vtable.kick_loop = kick_loop
  grpc_custom_pollset_init(&gevent_pollset_vtable)

  grpc_custom_pollset_set_init()
  grpc_custom_iomgr_init()


cdef grpc_error* grpc_error_none():
  return <grpc_error*>0
    
cdef grpc_error* socket_init(grpc_socket_wrapper* s, void* socket, int domain):
  if (<int>socket == 0):
    new_socket = gevent_socket.socket() # TODO domain
    s.socket = <void*> new_socket
    cpython.Py_INCREF(new_socket)
  else:
    cpython.Py_INCREF(<object>socket)
    s.socket = socket
  return grpc_error_none()

cdef addr_to_tuple(const sockaddr* addr, size_t addr_len):
  cdef char* res_str
  cdef grpc_resolved_address c_addr
  string.memcpy(<void*>c_addr.addr, <void*> addr, addr_len)
  c_addr.len = addr_len
  port = grpc_sockaddr_get_port(&c_addr)
  str_len = grpc_sockaddr_to_string(&res_str, &c_addr, 0) 
  if str_len < 0:
    raise Exception("FAILED")
  print("ADDR TO TUPLE")
  print(<bytes>res_str[:str_len])
  byte_str = <bytes>res_str[:str_len]
  if byte_str.endswith(':' + str(port)):
    byte_str = byte_str[:(0 - len(str(port)) - 1)]
  byte_str = byte_str.lstrip('[')
  byte_str = byte_str.rstrip(']')
  return (byte_str, port)

cdef socket_connect_async_cython(SocketWrapper socket_wrapper, addr_tuple):
  print("ATTEMPTING TO CONNECT")
  (<object>socket_wrapper.c_socket.socket).connect(addr_tuple)
  grpc_custom_connect_callback(<grpc_socket_wrapper*>socket_wrapper.c_socket, grpc_error_none())

def socket_connect_async(socket_wrapper, addr_tuple):
  print("SPAWNING CALLBACK")
  socket_connect_async_cython(socket_wrapper, addr_tuple)

cdef void socket_connect(grpc_socket_wrapper* s, const sockaddr* addr, size_t len):
  print("CLIENT CONNECT GEVENT")
  global g_greenlets
  socket_wrapper = SocketWrapper()
  socket_wrapper.c_socket = s
  addr_tuple = addr_to_tuple(addr, len)
  print("ADDR RESOLVED")
  g_greenlets.add(gevent_g.spawn(socket_connect_async, socket_wrapper, addr_tuple))

cdef void socket_destroy(grpc_socket_wrapper* s):
  cpython.Py_DECREF(<object>s.socket)

cdef void socket_shutdown(grpc_socket_wrapper* s):
  print("SHUTDOWN")
  (<object>s.socket).shutdown(gevent_socket.SHUT_RDWR)

cdef void socket_close(grpc_socket_wrapper* s):
  print("CLOSE")
  (<object>s.socket).close()
  grpc_custom_close_callback(s)

cdef socket_write_async_cython(SocketWrapper socket_wrapper, bytes):
  try:  
    ret = (<object>socket_wrapper.c_socket.socket).send(bytes)
    grpc_custom_write_callback(<grpc_socket_wrapper*>socket_wrapper.c_socket, ret, grpc_error_none())
  except IOError as e:
    if e.errno == errno.EPIPE:
      grpc_custom_write_callback(<grpc_socket_wrapper*>socket_wrapper.c_socket, -1, <grpc_error*>4)
    else:
      raise e
    

def socket_write_async(socket_wrapper, bytes):
  socket_write_async_cython(socket_wrapper, bytes)

cdef void socket_write(grpc_socket_wrapper* s, char* buffer, size_t length):
  global g_greenlets
  sw = SocketWrapper()
  sw.c_socket = s
  g_greenlets.add(gevent_g.spawn(socket_write_async, sw, buffer[:length]))

cdef socket_read_async_cython(SocketWrapper socket_wrapper):
  cdef char* buff_char_arr
  #TODO recv_into
  buff_str = (<object>socket_wrapper.c_socket.socket).recv(socket_wrapper.len)
  buff_char_arr = buff_str
  string.memcpy(<void*>socket_wrapper.c_buffer, buff_char_arr, len(buff_str))
  print(len(buff_str))
  grpc_custom_read_callback(<grpc_socket_wrapper*>socket_wrapper.c_socket, len(buff_str), grpc_error_none())


def socket_read_async(socket_wrapper):
  socket_read_async_cython(socket_wrapper)

cdef void socket_read(grpc_socket_wrapper* s, char* buffer, size_t length):
  global g_greenlets
  sw = SocketWrapper()
  sw.c_socket = s
  sw.c_buffer = buffer
  sw.len = length
  g_greenlets.add(gevent_g.spawn(socket_read_async, sw))

cdef grpc_error* socket_getpeername(grpc_socket_wrapper* s, sockaddr* addr, int* length):
  cdef char* src_buf
  peer = (<object>s.socket).getpeername()

  cdef grpc_resolved_address c_addr
  grpc_string_to_sockaddr(&c_addr, peer[0])
  grpc_sockaddr_set_port(&c_addr, peer[1])
  string.memcpy(<void*>addr, <void*>c_addr.addr, c_addr.len)
  length[0] = c_addr.len

  return grpc_error_none()  

cdef grpc_error* socket_getsockname(grpc_socket_wrapper* s, sockaddr* addr, int* length):
  cdef char* src_buf
  peer = (<object>s.socket).getsockname()
  print("GETSOCKNAME")
  print(peer[0])
  sys.stdout.flush()
  
  cdef grpc_resolved_address c_addr
  grpc_string_to_sockaddr(&c_addr, peer[0])
  grpc_sockaddr_set_port(&c_addr, peer[1])
  string.memcpy(<void*>addr, <void*>c_addr.addr, c_addr.len)
  length[0] = c_addr.len
  print(addr_to_tuple(addr, length[0]))
  return grpc_error_none()

cdef grpc_error* socket_setsockopt(grpc_socket_wrapper* s, int level, int optname,
                const void *optval, socklen_t optlen):
  cdef char* val_str = <char*> optval
  val_bytes = val_str[:optlen]
  (<object>s.socket).setsockopt(level, optname, val_bytes)
  return grpc_error_none()

cdef grpc_error* socket_bind(grpc_socket_wrapper* s, const sockaddr* addr, size_t len, int flags):
  print(addr_to_tuple(addr, len))
  sys.stdout.flush()
  (<object>s.socket).bind(addr_to_tuple(addr, len))
  return grpc_error_none()

cdef grpc_error* socket_listen(grpc_socket_wrapper* s):
  print("LISTEN")
  (<object>s.socket).listen(50)
  (<object>s.socket).setblocking(0)
  return grpc_error_none()

cdef object accept_callback_cython(SocketWrapper s):
  global g_greenlets
  print("START ACCEPT CALLBACK")
  s.event.set()
  print("EVEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE")
  print(s.event.__repr__())
  try:
    while True:
      try:
        conn, address = (<object>s.c_socket.socket).accept()
        print("ACCEPTING CONNECTION")
        cpython.Py_INCREF(conn)
        grpc_custom_accept_callback(<grpc_socket_wrapper*>s.c_socket, <void*>conn, grpc_error_none())
      except gevent_socket.error as err:
        if err.errno != errno.EWOULDBLOCK:
          raise err
        s.event = gevent_event.Event()
        print("NEW EVVVVVVVVVVEEEEEEEEEEEEEEE")
        print(s.event.__repr__())
        g_greenlets.add(s.event)
        return True
  except Exception as e:
    print("FAILURE")
    sys.stdout.flush()
    s.watcher.stop()
    #TODO actual error
    grpc_custom_accept_callback(<grpc_socket_wrapper*>s.c_socket, <void*>0, <grpc_error*>4)
    return False

def accept_callback(s):
  accept_callback_cython(s)

cdef grpc_error* socket_accept(grpc_socket_wrapper* s):
  py_socket = (<object>s.socket)
  watcher = gevent_g.get_hub().loop.io(py_socket.fileno(), 1)
  accept_event = gevent_event.Event()
  sw = SocketWrapper()
  sw.watcher = watcher
  sw.c_socket = s
  sw.event = accept_event
  watcher.start(accept_callback, sw)
  return grpc_error_none()

cdef class TimerWrapper:
  def __cinit__(self, deadline):
    self.timer = gevent_hub.get_hub().loop.timer(deadline)

  def start(self):
    self.timer.start(self.on_finish)

  def on_finish(self):
    grpc_custom_timer_callback(self.c_timer, grpc_error_none())
    self.timer.stop()

  def stop(self):
    self.timer.stop()

cdef grpc_error* timer_start(grpc_timer_wrapper* t):
  timer = TimerWrapper(t.timeout_ms / 1000.0)
  timer.c_timer = t
  t.timer = <void*>timer
  timer.start() 
  return grpc_error_none()

cdef grpc_error* timer_stop(grpc_timer_wrapper* t):
  time_wrapper = <object>t.timer
  time_wrapper.stop()
  return grpc_error_none()

cdef void kick_loop():
  print("KICK LOOP")
  sys.stdout.flush()
  gevent_hub.get_hub().loop.break_()

cdef void run_loop(int blocking):
  global g_greenlets
  print(g_greenlets)
  print("RUN LOOP %i %i" % (blocking, len(g_greenlets)))
  print(gevent_hub.get_hub().loop)
  sys.stdout.flush()
  if blocking:
    joined = gevent_g.wait(list(g_greenlets), count=1)
    print(joined)
    for elem in joined:
      g_greenlets.remove(elem)
  else:
    joined = gevent_g.wait(list(g_greenlets), count=1, timeout=0)
    print(joined)
    for elem in joined:
      g_greenlets.remove(elem)
