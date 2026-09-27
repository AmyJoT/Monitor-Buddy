#!/usr/bin/env python3
"""One-time Spotify login for Monitor-Buddy. Stdlib only. No client secret.

  python scripts/spotify_auth.py --client-id YOUR_CLIENT_ID
"""
import argparse
import base64
import hashlib
import http.server
import json
import secrets
import ssl
import string
import sys
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

REDIRECT_URI = "http://127.0.0.1:8888/callback"
SCOPES = "user-read-playback-state user-read-currently-playing user-modify-playback-state"
AUTH_URL = "https://accounts.spotify.com/authorize"
TOKEN_URL = "https://accounts.spotify.com/api/token"
UNRESERVED = string.ascii_letters + string.digits + "-._~"

def https_context():
    try:
        import certifi
    except ImportError:
        raise SystemExit("Missing SSL certificate bundle. Install it with: python -m pip install certifi")
    return ssl.create_default_context(cafile=certifi.where())

def code_verifier():
    return "".join(secrets.choice(UNRESERVED) for _ in range(64))

def code_challenge(verifier):
    digest = hashlib.sha256(verifier.encode("ascii")).digest()
    return base64.urlsafe_b64encode(digest).decode("ascii").rstrip("=")

class Callback(http.server.BaseHTTPRequestHandler):
    code = None
    error = None

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != "/callback":
            self.send_response(404)
            self.end_headers()
            return
        qs = urllib.parse.parse_qs(parsed.query)
        Callback.error = (qs.get("error") or [None])[0]
        Callback.code = (qs.get("code") or [None])[0]
        body = b"<html><body><h1>You can close this tab.</h1><p>Return to the terminal.</p></body></html>"
        self.send_response(200 if Callback.code else 400)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        return

def exchange(client_id, code, verifier):
    data = urllib.parse.urlencode({
        "grant_type": "authorization_code",
        "code": code,
        "redirect_uri": REDIRECT_URI,
        "client_id": client_id,
        "code_verifier": verifier,
    }).encode("ascii")
    req = urllib.request.Request(TOKEN_URL, data=data, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30, context=https_context()) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        try:
            err = json.loads(detail)
            detail = err.get("error_description") or err.get("error") or detail
        except json.JSONDecodeError:
            pass
        raise SystemExit("Spotify token error: " + detail)
    token = payload.get("refresh_token")
    if not token:
        raise SystemExit("Spotify did not return a refresh token.")
    return token

def main():
    parser = argparse.ArgumentParser(description="Spotify login for Monitor-Buddy")
    parser.add_argument("--client-id", required=True)
    args = parser.parse_args()
    verifier = code_verifier()
    params = urllib.parse.urlencode({
        "client_id": args.client_id,
        "response_type": "code",
        "redirect_uri": REDIRECT_URI,
        "scope": SCOPES,
        "code_challenge_method": "S256",
        "code_challenge": code_challenge(verifier),
    })
    url = AUTH_URL + "?" + params
    print("Opening the browser. If it does not, open this URL:")
    print(url)
    webbrowser.open(url)
    try:
        server = http.server.HTTPServer(("127.0.0.1", 8888), Callback)
    except OSError as exc:
        raise SystemExit("Could not listen on 127.0.0.1:8888: " + str(exc))
    server.timeout = 180
    print("Waiting up to 180 seconds...")
    server.handle_request()
    server.server_close()
    if Callback.error:
        raise SystemExit("Spotify error: " + Callback.error)
    if not Callback.code:
        raise SystemExit("Timed out waiting for the Spotify login.")
    refresh = exchange(args.client_id, Callback.code, verifier)
    print()
    print('#define SPOTIFY_CLIENT_ID     "%s"' % args.client_id)
    print('#define SPOTIFY_REFRESH_TOKEN "%s"' % refresh)
    print()
    print("Paste those two lines into config/config.h, set SHOW_SPOTIFY true, and reflash.")

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(1)