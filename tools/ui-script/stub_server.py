#!/usr/bin/env python3
"""MiyooFin UI-harness stub Jellyfin server (test-only, stdlib only).

Serves canned responses for exactly the endpoints the desktop build hits on
the smoke/series flows, on 127.0.0.1 with an ephemeral port:

   GET /System/Info/Public              fixed stub server identity
   POST /Users/AuthenticateByName        HTTP 400 with an empty body
                                         (login-400 regression flow)
   GET /Users/<uid>                       token validation -> 200
  GET /Users/<uid>/Views                  libraries (Movies + TV Shows)
  GET /Users/<uid>/Items?ParentId=...     library pages (movies / series)
  GET /Users/<uid>/Items/Resume           empty continue-watching rail
  GET /Users/<uid>/Items/Latest           empty recently-added rail
  GET /Shows/<series>/Seasons             two seasons
  GET /Shows/<series>/Episodes            a few episodes

Everything else (artwork, playback) -> 404, which the app
already treats as placeholder/empty. No network beyond loopback, no state,
no auth enforcement.

Usage: stub_server.py <portfile> [bind-address]   (binds an ephemeral
port, writes the port, serves). Default bind is 127.0.0.1 (desktop
runner and device runner alike); the device runner exposes the stub to
the Miyoo via a reverse SSH tunnel (device-127.0.0.1 -> host-127.0.0.1)
instead of a LAN bind — loopback-only, never exposed otherwise.
"""
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

FIXTURE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
N_POSTERS = 12  # fixtures/poster-N.jpg (portrait 2:3)
N_THUMBS = 6    # fixtures/thumb-N.jpg  (landscape 16:9)


def _slot(item_id, modulo):
    """Stable fixture slot for an id (not Python's randomised hash())."""
    h = 2166136261
    for ch in item_id.encode():
        h = ((h ^ ch) * 16777619) & 0xFFFFFFFF
    return h % modulo


def with_art(item, thumb=False):
    """Adds the ImageTags the app needs to request artwork for `item`."""
    tags = {"Primary": "p%d" % _slot(item["Id"], N_POSTERS)}
    if thumb:
        tags["Thumb"] = "t%d" % _slot(item["Id"], N_THUMBS)
    item["ImageTags"] = tags
    return item

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
# reads /Items/Latest. Five resume episodes and six latest movies exercise
# the Home rail geometry (five episode cards / six poster cards across the
# 640px framebuffer); the rails screenshot assertion is still the verdict.
RESUME = [
    {"Id": "ep-cw1", "Name": "Pilot", "Type": "Episode",
     "SeriesId": "series-testville", "SeriesName": "Testville",
     "SeasonId": "season-testville-1", "ParentIndexNumber": 1,
     "IndexNumber": 1, "RunTimeTicks": 18000000000,
     "UserData": {"Played": False, "PlaybackPositionTicks": 4000000000}},
    {"Id": "ep-cw2", "Name": "Second Helping", "Type": "Episode",
     "SeriesId": "series-testville", "SeriesName": "Testville",
     "SeasonId": "season-testville-1", "ParentIndexNumber": 1,
     "IndexNumber": 2, "RunTimeTicks": 18000000000,
     "UserData": {"Played": False, "PlaybackPositionTicks": 9000000000}},
    {"Id": "ep-cw3", "Name": "The Long Night", "Type": "Episode",
     "SeriesId": "series-testville", "SeriesName": "Testville",
     "SeasonId": "season-testville-2", "ParentIndexNumber": 2,
     "IndexNumber": 1, "RunTimeTicks": 18000000000,
     "UserData": {"Played": False, "PlaybackPositionTicks": 13500000000}},
    {"Id": "ep-cw4", "Name": "Returns", "Type": "Episode",
     "SeriesId": "series-testville", "SeriesName": "Testville",
     "SeasonId": "season-testville-2", "ParentIndexNumber": 2,
     "IndexNumber": 2, "RunTimeTicks": 18000000000,
     "UserData": {"Played": False, "PlaybackPositionTicks": 2000000000}},
    {"Id": "ep-cw5", "Name": "Finale", "Type": "Episode",
     "SeriesId": "series-testville", "SeriesName": "Testville",
     "SeasonId": "season-testville-2", "ParentIndexNumber": 2,
     "IndexNumber": 3, "RunTimeTicks": 18000000000,
     "UserData": {"Played": True, "PlaybackPositionTicks": 0}},
]
LATEST = [
    {"Id": "movie-1", "Name": "Stub Movie One", "Type": "Movie",
     "Overview": "First stub movie.", "ProductionYear": 2021,
     "RunTimeTicks": 54000000000, "UserData": {"Played": False}},
    {"Id": "movie-2", "Name": "Stub Movie Two", "Type": "Movie",
     "Overview": "Second stub movie.", "ProductionYear": 2022,
     "RunTimeTicks": 60000000000, "UserData": {"Played": True}},
    {"Id": "movie-3", "Name": "Stub Movie Three", "Type": "Movie",
     "Overview": "Third stub movie.", "ProductionYear": 2023,
     "RunTimeTicks": 60000000000, "UserData": {"Played": False}},
    {"Id": "movie-4", "Name": "Stub Movie Four", "Type": "Movie",
     "Overview": "Fourth stub movie.", "ProductionYear": 2024,
     "RunTimeTicks": 60000000000, "UserData": {"Played": False}},
    {"Id": "movie-5", "Name": "Stub Movie Five", "Type": "Movie",
     "Overview": "Fifth stub movie.", "ProductionYear": 2019,
     "RunTimeTicks": 60000000000, "UserData": {"Played": False}},
    {"Id": "movie-6", "Name": "Stub Movie Six", "Type": "Movie",
     "Overview": "Sixth stub movie.", "ProductionYear": 2020,
     "RunTimeTicks": 60000000000, "UserData": {"Played": False}},
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

MORE_MOVIES = [
    ("Midnight Harbor", 2023, 7.9, "Thriller", 7200), ("The Glass Orchard", 2022, 6.8, "Drama", 6300),
    ("Paper Moons", 2021, 8.3, "Romance", 5700), ("Iron Lantern", 2024, 7.1, "Action", 8100),
    ("Quiet Satellites", 2019, 8.0, "Sci-Fi", 7800), ("Salt & Ember", 2020, 6.5, "Drama", 6600),
    ("Northbound", 2018, 7.4, "Adventure", 7500), ("Velvet Static", 2022, 7.7, "Mystery", 6900),
    ("Hollow Crown Hill", 2017, 6.9, "Fantasy", 8400), ("A Long Way Down", 2016, 7.2, "Drama", 6000),
    ("Neon Cathedral", 2024, 8.4, "Sci-Fi", 7200), ("The Last Lighthouse", 2015, 7.6, "Adventure", 5400),
    ("Brass Kingdom", 2023, 6.4, "Comedy", 5100), ("Wolves of Winter Street", 2021, 7.0, "Crime", 7000),
    ("Echo Valley", 2020, 7.8, "Western", 7700), ("Marigold", 2019, 8.1, "Romance", 5900),
    ("Deep Field", 2025, 8.6, "Sci-Fi", 8800), ("The Cartographer", 2014, 7.3, "Mystery", 6400),
    ("Sunday Static", 2022, 6.6, "Comedy", 5200), ("Ghost Light", 2018, 7.5, "Horror", 6100),
    ("Tin Soldier Blues", 2017, 6.7, "War", 7400), ("Orbit of Seven", 2024, 8.2, "Sci-Fi", 8000),
]
for _i, (_name, _year, _rating, _genre, _secs) in enumerate(MORE_MOVIES, start=3):
    MOVIES.append({"Id": "movie-%d" % _i, "Name": _name, "Type": "Movie",
                   "Overview": "%s is a %s from %d for the UI harness." % (_name, _genre.lower(), _year),
                   "ProductionYear": _year, "CommunityRating": _rating, "Genres": [_genre],
                   "RunTimeTicks": _secs * 10000000, "UserData": {"Played": _i % 5 == 0}})
for _m in MOVIES:
    with_art(_m)
for _m in LATEST:
    with_art(_m)

MORE_SERIES = [("Harbor Lights", 2022, 8.4, "Drama"), ("The Long Room", 2021, 7.9, "Mystery"),
               ("Copper Falls", 2019, 8.7, "Crime"), ("Starboard", 2023, 7.2, "Sci-Fi"),
               ("Second Spring", 2020, 7.5, "Comedy"), ("Dead Reckoning", 2018, 8.1, "Adventure"),
               ("Kettle & Crow", 2024, 7.0, "Fantasy")]
for _name, _year, _rating, _genre in MORE_SERIES:
    SERIES.append({"Id": "series-" + _name.lower().replace(" ", "-").replace("&", "and"), "Name": _name,
                   "Type": "Series", "Overview": "%s: a %s series for the UI harness." % (_name, _genre.lower()),
                   "ProductionYear": _year, "CommunityRating": _rating, "Genres": [_genre],
                   "UserData": {"Played": False}})
for _s in SERIES:
    with_art(_s)
for _e in RESUME:
    with_art(_e, thumb=True)
for _e in EPISODES:
    with_art(_e, thumb=True)
# Mixed content on the Continue Watching rail: two movies with progress.
RESUME.insert(1, with_art({"Id": "movie-9", "Name": "Northbound", "Type": "Movie",
                           "ProductionYear": 2018, "RunTimeTicks": 75000000000,
                           "UserData": {"Played": False, "PlaybackPositionTicks": 30000000000}}))
RESUME.append(with_art({"Id": "movie-17", "Name": "Deep Field", "Type": "Movie",
                        "ProductionYear": 2025, "RunTimeTicks": 88000000000,
                        "UserData": {"Played": False, "PlaybackPositionTicks": 12000000000}}))
LATEST[:] = LATEST + [m for m in MOVIES if m["Id"] in ("movie-7", "movie-8", "movie-11", "movie-13")]


def seasons_for(series_id):
    if series_id == "series-testville":
        return SEASONS
    name = next((s["Name"] for s in SERIES if s["Id"] == series_id), "Series")
    return [with_art({"Id": "season-%s-%d" % (series_id, n), "Name": "Season %d" % n, "Type": "Season",
                      "SeriesId": series_id, "SeriesName": name, "IndexNumber": n,
                      "UserData": {"Played": False}}) for n in (1, 2, 3)]


def episodes_for(series_id):
    if series_id == "series-testville":
        return EPISODES
    name = next((s["Name"] for s in SERIES if s["Id"] == series_id), "Series")
    out = []
    for season in (1, 2, 3):
        for ep in range(1, 7):
            out.append(with_art({"Id": "ep-%s-s%de%d" % (series_id, season, ep), "Name": "Episode %d" % ep,
                                 "Type": "Episode", "SeriesId": series_id, "SeriesName": name,
                                 "SeasonId": "season-%s-%d" % (series_id, season),
                                 "ParentIndexNumber": season, "IndexNumber": ep,
                                 "RunTimeTicks": 27000000000, "UserData": {"Played": ep < 3}}, thumb=True))
    return out


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

    def _send_raw(self, code, body):
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
        elif path == "/System/Info/Public":
            # ConnectScreen/ServerEntryScreen probe. A fixed name keeps the
            # login-400 harness deterministic.
            self._send(200, {"Id": "stub-server", "ServerName": "Stub",
                             "Version": "10.9.0",
                             "OperatingSystem": "stub"})
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
            self._send(200, page(seasons_for(path.split("/")[2])))
        elif path.startswith("/Shows/") and path.endswith("/Episodes"):
            eps = episodes_for(path.split("/")[2])
            wanted = qs.get("SeasonId", [""])[0]
            self._send(200, page([e for e in eps if not wanted or e["SeasonId"] == wanted]))
        elif path.startswith("/Items/") and "/Images/" in path:
            # Fixture artwork: portrait posters for Primary, landscape for Thumb.
            parts = path.split("/")
            kind = parts[4] if len(parts) > 4 else "Primary"
            thumb = kind in ("Thumb", "Backdrop")
            name = "thumb-%d.jpg" % _slot(parts[2], N_THUMBS) if thumb else "poster-%d.jpg" % _slot(parts[2], N_POSTERS)
            with open(os.path.join(FIXTURE_DIR, name), "rb") as handle:
                body = handle.read()
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self._send(404, {"error": "stub has no such endpoint"})

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path == "/Users/AuthenticateByName":
            # Regression pin for the login-400 report: HTTP 400 with an
            # empty body. Consume the request body so the connection stays
            # in sync, then answer with zero bytes.
            length = int(self.headers.get("Content-Length") or 0)
            if length > 0:
                self.rfile.read(length)
            self._send_raw(400, b"")
            return
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
