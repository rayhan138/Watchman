"""Disposable runner: serve only fixture bytes over local, certificate-validated TLS."""
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import ssl
import sys

root, cert, key = sys.argv[1:]
class Fixture(SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path.endswith('/latest.json') and (Path(root) / 'redirect-test').exists():
            self.send_response(302)
            self.send_header('Location', 'http://localhost:45443/not-https')
            self.end_headers()
        else:
            super().do_GET()


server = ThreadingHTTPServer(('127.0.0.1', 45443), partial(Fixture, directory=root))
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(cert, key)
server.socket = context.wrap_socket(server.socket, server_side=True)
server.serve_forever()
