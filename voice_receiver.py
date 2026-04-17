#!/usr/bin/env python3
"""
Voice data receiver for ReVoice CS 1.6 plugin.

Receives WAV files uploaded by the rv_upload_dump server command
and saves them to disk preserving the original directory structure.

On the game server (revoice.cfg):
    REV_UploadURL "http://your-machine-ip:5000/upload"

Then in the game server console:
    rv_upload_dump
"""

# ── Configuration ─────────────────────────────────────────────
PORT = 5000
UPLOAD_DIR = './voice_data'
ALLOWED_IPS = ['46.174.52.2']
# ──────────────────────────────────────────────────────────────

import os
import subprocess
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn


class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True


class VoiceUploadHandler(BaseHTTPRequestHandler):
    upload_dir = UPLOAD_DIR
    allowed_ips = set(ALLOWED_IPS)

    def do_POST(self):
        client_ip = self.client_address[0]
        if self.allowed_ips and client_ip not in self.allowed_ips:
            self.send_error(403, 'Forbidden')
            return

        if self.path != '/upload':
            self.send_error(404, 'Not Found')
            return

        filepath = self.headers.get('X-Filepath', '')
        if not filepath:
            self.send_error(400, 'Missing X-Filepath header')
            return

        # Sanitize path to prevent traversal attacks
        filepath = filepath.replace('\\', '/')
        filepath = os.path.normpath(filepath)
        if filepath.startswith('..') or filepath.startswith('/'):
            self.send_error(400, 'Invalid filepath')
            return

        content_length = int(self.headers.get('Content-Length', 0))
        if content_length <= 0:
            self.send_error(400, 'Empty body')
            return

        body = self.rfile.read(content_length)

        save_path = os.path.join(self.upload_dir, filepath)
        os.makedirs(os.path.dirname(save_path), exist_ok=True)

        with open(save_path, 'wb') as f:
            f.write(body)

        size_kb = len(body) / 1024.0
        print(f'[{client_ip}] Saved: {filepath} ({size_kb:.1f} KB)')

        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        self.end_headers()
        self.wfile.write(b'OK')

    def log_message(self, format, *args):
        pass


def main():
    os.makedirs(UPLOAD_DIR, exist_ok=True)

    server = ThreadedHTTPServer(('0.0.0.0', PORT), VoiceUploadHandler)
    print(f'Listening on 0.0.0.0:{PORT}')
    print(f'Save directory: {os.path.abspath(UPLOAD_DIR)}')
    if ALLOWED_IPS:
        print(f'Allowed IPs: {", ".join(ALLOWED_IPS)}')
    else:
        print('WARNING: Accepting uploads from any IP')

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('\nShutting down')
        server.shutdown()


if __name__ == '__main__':
    main()
