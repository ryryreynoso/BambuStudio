#!/usr/bin/env python3
"""Stand-in for Snapmaker's connection helper (snapmaker_control_*), for testing Bambu Studio's
SnapmakerConnect without Snapmaker's program.

Behaves like the helper as Snapmaker Orca uses it: started with --locale=<code> --orca, it serves
HTTP on a free 127.0.0.1 port, prints "PORT:<n>\\r\\n\\r\\n" on stdout, and answers /api/health,
/api/account and two pages. The account reports signed in when the environment variable
FAKE_SNAPMAKER_SIGNED_IN is set. It talks to nothing outside the local machine.
"""
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

locale = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--locale=")), "")
if "--orca" not in sys.argv[1:] or not locale:
    sys.exit(2)


class Handler(BaseHTTPRequestHandler):
    def reply(self, code, payload, content_type="application/json"):
        body = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        port = self.server.server_address[1]
        if self.path == "/api/health":
            self.reply(200, {"status": "ok", "cli_version": "fake-1.0", "components": {"ipc_server": "ok", "web_server": "ok"},
                             "server_url": {"base_url": "http://127.0.0.1:%d" % port, "home_page": "/web/home?locale=" + locale,
                                            "device_control": "/web/devices"}})
        elif self.path == "/api/account":
            if os.environ.get("FAKE_SNAPMAKER_SIGNED_IN"):
                self.reply(200, {"is_login": True, "userid": "42", "nickname": "Tester", "token": "secret-token"})
            else:
                self.reply(200, {"is_login": False})
        elif self.path.startswith("/web/"):
            self.reply(200, b"<html><body>fake helper page</body></html>", "text/html")
        else:
            self.reply(404, {"error": "not found"})

    def log_message(self, *args):
        pass


server = HTTPServer(("127.0.0.1", 0), Handler)
sys.stdout.write("PORT:%d\r\n\r\n" % server.server_address[1])
sys.stdout.flush()
server.serve_forever()
