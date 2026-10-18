"""A small REST API for controlling a running overlord, e.g. from an SSH session.

    curl localhost:8080/status
    curl -X POST localhost:8080/volume/80

Routes are decided by overlord.py; this file only carries requests to it and
replies as JSON. There is no authentication, so it listens on localhost unless
told otherwise.
"""

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit


class ControlServer:
    def __init__(self, handle, host="127.0.0.1", port=8080):
        """handle(method, parts) -> (http_status, dict). parts is the URL path split
        on '/', so /volume/80 is ["volume", "80"]."""

        class Handler(BaseHTTPRequestHandler):
            def _serve(self):
                parts = [unquote(p) for p in urlsplit(self.path).path.split("/") if p]
                try:
                    status, body = handle(self.command, parts)
                except Exception as e:  # a bad request must not take the server down
                    status, body = 500, {"error": str(e)}
                data = (json.dumps(body) + "\n").encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            do_GET = do_POST = do_PUT = _serve

            def log_message(self, *args):  # overlord logs what the request did
                pass

        self._httpd = ThreadingHTTPServer((host, port), Handler)
        self._httpd.daemon_threads = True
        self.address = self._httpd.server_address

    def start(self):
        threading.Thread(target=self._httpd.serve_forever, daemon=True).start()

    def stop(self):
        self._httpd.shutdown()
        self._httpd.server_close()
