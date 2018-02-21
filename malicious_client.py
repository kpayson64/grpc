import ssl
import sys
import socket
import time

PREFIX = (
  "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" +
  "\x00\x00\x00\x04\x00\x00\x00\x00\x00")

HEADER = (
  "\x00\x00\xee\x01\x04\x00\x00\x00\x01"
  "\x10\x05:path#/grpc.testing.TestService/UnaryCall"
  "\x10\x07:scheme\x04http"
  "\x10\x07:method\x04POST"
  "\x10\x0a:authority\x13test.googleapis.com"
  "\x10\x0c""content-type\x10""application/grpc"
  "\x10\x14grpc-accept-encoding\x15identity,deflate,gzip"
  "\x10\x02te\x08trailers"
  "\x10\x0auser-agent\"bad-client grpc-c/0.12.0.0 (linux)")

PAYLOAD = (
  "\x00\x00\x20\x00\x00\x00\x01\x00\x01" +
  "\x00\x00\x00\x00")

port = int(sys.argv[1])
s = socket.socket()
#s = ssl.wrap_socket(s)
s.connect(('localhost', port))
s.send(PREFIX + HEADER + PAYLOAD + HEADER)
time.sleep(0.5)
print s.recv(10000)
s.close()

