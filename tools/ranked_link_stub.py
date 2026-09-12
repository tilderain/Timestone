#!/usr/bin/env python3
"""Local stand-in for the future Timestone link service (prod: login.hero-rehab.xyz).

The client (ranked_account.cpp) is pointed at this stub with
[Settings] CustomRankedServer = http://127.0.0.1:8787 to point the whole link
flow (page + API) at this stub. [Settings] RankedBaseUrl is the narrower
API-only override; either key alone points everything here.

Flow the operator drives in a browser:
  1. The game opens  http://127.0.0.1:8787/link?app=umvc3&steam_id=...&state=X
  2. Click "simulate link completion" on that page (hits /done?state=X), or
     open /done?state=X directly with the same state.
  3. The game's poll thread sees {"status":"linked"}, POSTs /link/complete,
     and this stub prints `COMPLETE <json body>`.

Run:  python3 tools/ranked_link_stub.py [port]     (default 127.0.0.1:8787)
"""

import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

LINKED_STATES = {}  # state -> True once the operator simulates completion

PAGE = """<!doctype html><html><head><meta charset="utf-8"><title>Timestone link</title></head>
<body style="font-family:sans-serif;margin:4em auto;max-width:36em">
<h1>Linking in progress</h1>
<p>Complete the Discord step above, then return to the game.</p>
<p>This is the local test stub for the future Timestone link service.
Click below to simulate the real service completing the link:</p>
<form action="/done" method="get">
  <input type="hidden" name="state" value="{state}">
  <button type="submit" style="font-size:1.1em">Simulate link completion</button>
</form>
</body></html>"""


class Handler(BaseHTTPRequestHandler):
    def _send(self, status, content_type, body):
        data = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)
        state = (query.get("state") or [""])[0]
        if parsed.path == "/link":
            self._send(200, "text/html; charset=utf-8",
                       PAGE.format(state=state or ""))
        elif parsed.path == "/done":
            if not state:
                self._send(400, "text/plain; charset=utf-8", "missing state")
                return
            LINKED_STATES[state] = True
            print(f"DONE state={state}")
            self._send(200, "text/html; charset=utf-8",
                       "<p>Link marked complete. Return to the game.</p>")
        elif parsed.path == "/link/status":
            status = "linked" if LINKED_STATES.get(state) else "pending"
            self._send(200, "application/json",
                       json.dumps({"status": status}, separators=(",", ":")))
        else:
            self._send(404, "text/plain; charset=utf-8", "not found")

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path != "/link/complete":
            self._send(404, "text/plain; charset=utf-8", "not found")
            return
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length).decode("utf-8", "replace")
        print(f"COMPLETE {body}")
        self._send(200, "application/json", json.dumps({"ok": True}, separators=(",", ":")))

    def log_message(self, fmt, *args):  # silence per-request noise
        pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8787
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"ranked link stub listening on http://127.0.0.1:{port}")
    print("Simulate completion with:  curl 'http://127.0.0.1:%d/done?state=X'\n"
          % port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
