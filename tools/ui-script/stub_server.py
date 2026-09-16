#!/usr/bin/env python3
"""MiyooFin UI-harness stub Jellyfin server (test-only, stdlib only).

Serves canned responses for exactly the endpoints the desktop build hits on
the smoke/series flows, on 127.0.0.1 with an ephemeral port:

  GET /Users/<uid>                       token validation -> 200
  GET /Users/<uid>/Views                  libraries (Movies + TV Shows)
  GET /Users/<uid>/Items?ParentId=...     library pages (movies / series)
  GET /Users/<uid>/Items/Resume           empty continue-watching rail
  GET /Users/<uid>/Items/Latest           empty recently-added rail
  GET /Shows/<series>/Seasons             two seasons
  GET /Shows/<series>/Episodes            a few episodes

Everything else (artwork, playback, system info) -> 404, which the app
already treats as placeholder/empty. No network beyond loopback, no state,
no auth enforcement.

Usage: stub_server.py <portfile> [bind-address]   (binds an ephemeral
port, writes the port, serves). Default bind is 127.0.0.1 (desktop
runner and device runner alike); the device runner exposes the stub to
the Miyoo via a reverse SSH tunnel (device-127.0.0.1 -> host-127.0.0.1)
instead of a LAN bind — loopback-only, never exposed otherwise.
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

UID = "user-stub"

MOVIES = [
    {"Id": "movie-1", "Name": "Stub Movie One", "Type": "Movie",
     "Overview": "First stub movie.", "ProductionYear": 2021,
     "CommunityRating": 7.5, "Genres": ["Action"],
     "RunTimeTicks": 54000000000,
     "UserData": {"Played": False}},
    {"Id": "movie-2", "Name": "Stub Movie Two", "Type": "Movie",
     "Overview": "Second stub movie.", "ProductionYear": 2022,
     "CommunityRating": 8.1, "Genres": ["Drama"],
     "RunTimeTicks": 60000000000,
     "UserData": {"Played": True}},
]

# Home-tab rails: Continue Watching reads /Items/Resume, Recently Added
# reads /Items/Latest. Non-empty rails prove the Home tab renders lists.
RESUME = [
    {"Id": "movie-1", "Name": "Stub Movie One", "Type": "Movie",
     "Overview": "First stub movie.", "ProductionYear": 2021,
     "RunTimeTicks": 54000000000,
     "UserData": {"Played": False, "PlaybackPositionTicks": 18000000000}},
]
LATEST = [
    {"Id": "movie-2", "Name": "Stub Movie Two", "Type": "Movie",
     "Overview": "Second stub movie.", "ProductionYear": 2022,
     "RunTimeTicks": 60000000000,
     "UserData": {"Played": False}},
]

SERIES = [
    {"Id": "series-testville", "Name": "Testville", "Type": "Series",
     "Overview": "A stub series for UI-harness navigation.",
     "ProductionYear": 2020, "CommunityRating": 8.8,
     "Genres": ["Comedy"], "UserData": {"Played": False}},
]

SEASONS = [
    {"Id": "season-testville-1", "Name": "Season 1", "Type": "Season",
     "SeriesId": "series-testville", "SeriesName": "Testville",
     "IndexNumber": 1, "UserData": {"Played": False}},
    {"Id": "season-testville-2", "Name": "Season 2", "Type": "Season",
     "SeriesId": "series-testville", "SeriesName": "Testville",
     "IndexNumber": 2, "UserData": {"Played": False}},
]

EPISODES = [
    {"Id": "ep-s1e1", "Name": "Pilot", "Type": "Episode",
     "SeriesId": "series-testville", "SeriesName": "Testville",
     "SeasonId": "season-testville-1", "ParentIndexNumber": 1,
     "IndexNumber": 1, "UserData": {"Played": False}},
    {"Id": "ep-s1e2", "Name": "Second Helping", "Type": "Episode",
     "SeriesId": "series-testville", "SeriesName": "Testville",
     "SeasonId": "season-testville-1", "ParentIndexNumber": 1,
     "IndexNumber": 2, "UserData": {"Played": False}},
    {"Id": "ep-s2e1", "Name": "Returns", "Type": "Episode",
     "SeriesId": "series-testville", "SeriesName": "Testville",
     "SeasonId": "season-testville-2", "ParentIndexNumber": 2,
     "IndexNumber": 1, "UserData": {"Played": False}},
]

VIEWS = [
    {"Id": "view-movies", "Name": "Movies", "CollectionType": "movies"},
    {"Id": "view-shows", "Name": "TV Shows", "CollectionType": "tvshows"},
]


def page(items):
    return {"Items": items, "TotalRecordCount": len(items),
            "StartIndex": 0}


class Handler(BaseHTTPRequestHandler):
    server_version = "StubJellyfin/1"

    def log_message(self, fmt, *args):  # keep stderr for real errors
        sys.stderr.write("stub: " + fmt % args + "\n")

    def _send(self, code, obj=None):
        body = json.dumps(obj).encode() if obj is not None else b""
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        qs = parse_qs(parsed.query)
        if path == "/healthz":
            # Reachability probe for the device harness. The probe MUST get a
            # 2xx: it runs `wget -q`, and wget exits 8 on ANY HTTP error
            # response, so probing a 404 path would report a perfectly good
            # tunnel as unreachable.
            self._send(200, {"status": "ok"})
        elif path == "/Users/%s" % UID:
            self._send(200, {"Id": UID, "Name": "stub"})
        elif path == "/Users/%s/Views" % UID:
            self._send(200, page(VIEWS))
        elif path == "/Users/%s/Items/Resume" % UID:
            self._send(200, page(RESUME))
        elif path == "/Users/%s/Items/Latest" % UID:
            self._send(200, page(LATEST))
        elif path == "/Users/%s/Items" % UID:
            parent = (qs.get("ParentId") or [""])[0]
            if parent == "view-movies":
                self._send(200, page(MOVIES))
            elif parent == "view-shows":
                self._send(200, page(SERIES))
            else:
                self._send(200, page([]))
        elif path.startswith("/Shows/") and path.endswith("/Seasons"):
            self._send(200, page(SEASONS))
        elif path.startswith("/Shows/") and path.endswith("/Episodes"):
            self._send(200, page(EPISODES))
        else:
            self._send(404, {"error": "stub has no such endpoint"})

    def do_POST(self):
        self._send(404, {"error": "stub has no such endpoint"})


def main():
    portfile = sys.argv[1]
    bind = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
    server = HTTPServer((bind, 0), Handler)
    with open(portfile, "w") as f:
        f.write(str(server.server_address[1]))
    sys.stderr.write("stub: listening on %s:%d\n"
                     % (bind, server.server_address[1]))
    server.serve_forever()


if __name__ == "__main__":
    main()
