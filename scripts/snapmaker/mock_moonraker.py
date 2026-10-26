#!/usr/bin/env python3
"""Minimal stand-in for Moonraker's HTTP API, for testing Bambu Studio's Moonraker print host.

Answers GET /printer/info and POST /server/files/upload like Moonraker does and records every
upload (form fields and file size) to <log.json>. Nothing is printed or forwarded anywhere.

usage: mock_moonraker.py <port> <log.json>
"""
import json
from email.parser import BytesParser
from email.policy import HTTP
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT, LOG = int(sys.argv[1]), sys.argv[2]
uploads = []


class Handler(BaseHTTPRequestHandler):
    def reply(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/printer/info":
            self.reply(200, {"result": {"state": "ready", "hostname": "mock-u1", "software_version": "mock"}})
        else:
            self.reply(404, {"error": {"code": 404, "message": "Not Found"}})

    def do_POST(self):
        if self.path != "/server/files/upload":
            self.reply(404, {"error": {"code": 404, "message": "Not Found"}})
            return
        body = self.rfile.read(int(self.headers["Content-Length"]))
        message = BytesParser(policy=HTTP).parsebytes(b"Content-Type: " + self.headers["Content-Type"].encode() + b"\r\n\r\n" + body)
        fields, filename, data = {}, None, b""
        for part in message.iter_parts():
            name = part.get_param("name", header="content-disposition")
            if name == "file":
                filename, data = part.get_filename(), part.get_payload(decode=True)
            else:
                fields[name] = part.get_payload(decode=True).decode()
        record = {k: fields.get(k) for k in ("root", "path", "print")}
        record.update({"filename": filename, "size": len(data), "first_line": data.split(b"\n", 1)[0].decode(errors="replace")})
        uploads.append(record)
        json.dump(uploads, open(LOG, "w"), indent=2)
        self.reply(201, {"result": {"item": {"path": filename, "root": record["root"]},
                                    "print_started": record["print"] == "true", "action": "create_file"}})

    def log_message(self, *args):
        pass


HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
