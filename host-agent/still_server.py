"""Phase 0 still receiver: the Watcher's ``snap`` command HTTP POSTs a JPEG here.

Uses only the stdlib (http.server in a background thread) — no web framework.

Endpoints:
  POST /still?id=N   raw JPEG body -> saved as still_<id>_<timestamp>.jpg
                     in out_dir; on_still(path, id) is called afterwards.
  GET  /health       -> 200 "ok" (liveness check)
"""

from __future__ import annotations

import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Callable, Optional
from urllib.parse import urlparse, parse_qs

MAX_STILL_BYTES = 8 * 1024 * 1024  # sanity cap per JPEG


class StillServer:
    """Threaded HTTP server that receives JPEG stills from the Watcher."""

    def __init__(
        self,
        host: str = "0.0.0.0",
        port: int = 8099,
        out_dir: str = "stills",
        on_still: Optional[Callable[[str, int], None]] = None,
    ):
        self.host = host
        self.port = port
        self.out_dir = out_dir
        self.on_still = on_still
        os.makedirs(self.out_dir, exist_ok=True)
        self._handler_cls = self._make_handler()
        self._httpd: Optional[ThreadingHTTPServer] = None
        self._thread: Optional[threading.Thread] = None

    # -- request handling --------------------------------------------------

    def _make_handler(self):
        server = self

        class StillHandler(BaseHTTPRequestHandler):
            server_version = "WatcherStill/0"

            def log_message(self, fmt, *args):  # keep quiet; caller logs saves
                pass

            def _send(self, code: int, body: bytes, ctype: str = "text/plain") -> None:
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):  # noqa: N802 (http.server naming)
                path = urlparse(self.path).path
                if path == "/health":
                    self._send(200, b"ok")
                else:
                    self._send(404, b"not found")

            def do_POST(self):  # noqa: N802 (http.server naming)
                parsed = urlparse(self.path)
                if parsed.path != "/still":
                    self._send(404, b"not found")
                    return
                ids = parse_qs(parsed.query).get("id", [])
                if not ids or not ids[0].isdigit():
                    self._send(400, b"missing or invalid ?id=N")
                    return
                still_id = int(ids[0])
                try:
                    length = int(self.headers.get("Content-Length", ""))
                except (TypeError, ValueError):
                    length = -1
                if length < 0:
                    self._send(411, b"Content-Length required")
                    return
                if length > MAX_STILL_BYTES:
                    self._send(413, b"body too large")
                    return
                body = self.rfile.read(length)
                timestamp = time.strftime("%Y%m%d_%H%M%S")
                filename = f"still_{still_id}_{timestamp}.jpg"
                path = os.path.join(server.out_dir, filename)
                with open(path, "wb") as fh:
                    fh.write(body)
                if server.on_still is not None:
                    try:
                        server.on_still(path, still_id)
                    except Exception:
                        pass  # a callback must never break the receiver
                self._send(200, b"saved")

        return StillHandler

    # -- lifecycle ---------------------------------------------------------

    def start(self) -> int:
        """Start serving in a daemon thread. Returns the bound port."""
        if self._httpd is not None:
            raise RuntimeError("StillServer already started")
        self._httpd = ThreadingHTTPServer((self.host, self.port), self._handler_cls)
        self._httpd.daemon_threads = True
        self.port = self._httpd.server_address[1]  # honours port=0
        self._thread = threading.Thread(
            target=self._httpd.serve_forever, name="watcher-still-server", daemon=True
        )
        self._thread.start()
        return self.port

    def stop(self) -> None:
        if self._httpd is not None:
            self._httpd.shutdown()
            self._httpd.server_close()
            self._httpd = None
        if self._thread is not None:
            self._thread.join(timeout=5.0)
            self._thread = None

    def __enter__(self) -> "StillServer":
        self.start()
        return self

    def __exit__(self, *exc_info) -> None:
        self.stop()
