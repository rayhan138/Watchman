"""Disposable runner: serve only fixture bytes over local, certificate-validated TLS."""
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import ssl
import sys

root, cert, key = sys.argv[1:]
server = ThreadingHTTPServer(('127.0.0.1', 45443), partial(SimpleHTTPRequestHandler, directory=root))
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(cert, key)
server.socket = context.wrap_socket(server.socket, server_side=True)
server.serve_forever()
