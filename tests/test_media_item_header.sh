#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
printf '%s\n' '#include "src/data/MediaItem.hpp"' 'int main() { miyoofin::MediaItem item; return item.year; }' > "$TMP/check.cpp"
c++ -std=c++17 -I"$ROOT" -fsyntax-only "$TMP/check.cpp"
if grep -q 'SDL\|Uint8\|MediaRow\|TabData' "$ROOT/src/data/MediaItem.hpp"; then
    echo 'MediaItem domain header contains UI/SDL coupling' >&2
    exit 1
fi
if grep -q 'PlaceholderArtwork\|placeholderArtwork\|art_[rgb]' "$ROOT/src/data/MediaItem.hpp"; then
    echo 'MediaItem domain header contains presentation RGB state' >&2
    exit 1
fi
echo 'MediaItem domain header structural check passed'
