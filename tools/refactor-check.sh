#!/bin/sh
set -eu

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
repo_root=$(CDPATH= cd "$script_dir/.." && pwd)
cd "$repo_root"

# No first-party production worker may be detached: detached threads can
# outlive the owner that their callbacks capture.  Scan tracked C/C++ sources
# only, mask comments and literals to avoid prose/data false positives, and
# exclude imported/generated trees consistently with format-check.
git ls-files -z -- 'src/**' | python3 -c '
import os
import re
import sys

extensions = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
excluded_directories = {
    "external", "generated", "imported", "third-party", "third_party", "vendor",
}

def mask_non_code(text):
    masked = list(text)
    i = 0
    size = len(text)
    while i < size:
        if text.startswith("//", i):
            end = text.find("\n", i)
            end = size if end < 0 else end
            for position in range(i, end):
                masked[position] = " "
            i = end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = size if end < 0 else end + 2
            for position in range(i, end):
                if text[position] != "\n":
                    masked[position] = " "
            i = end
        elif text.startswith("R\"", i):
            delimiter_end = text.find("(", i + 2)
            if delimiter_end < 0:
                i += 2
                continue
            delimiter = text[i + 2:delimiter_end]
            terminator = ")" + delimiter + "\""
            end = text.find(terminator, delimiter_end + 1)
            end = size if end < 0 else end + len(terminator)
            for position in range(i, end):
                if text[position] != "\n":
                    masked[position] = " "
            i = end
        elif text[i] in (chr(34), chr(39)):
            quote = text[i]
            i += 1
            while i < size:
                if text[i] == "\\":
                    masked[i] = " "
                    if i + 1 < size and text[i + 1] != "\n":
                        masked[i + 1] = " "
                    i += 2
                else:
                    escaped = text[i] == quote
                    masked[i] = " "
                    i += 1
                    if escaped:
                        break
        else:
            i += 1
    return "".join(masked)

patterns = (
    re.compile(r"\.\s*detach\s*\("),
    re.compile(r"\bpthread_detach\s*\("),
)
failed = False
for raw_path in sys.stdin.buffer.read().split(b"\0"):
    if not raw_path:
        continue
    path = os.fsdecode(raw_path)
    parts = path.split("/")
    if not any(path.endswith(extension) for extension in extensions):
        continue
    if any(part.casefold() in excluded_directories for part in parts[1:]):
        continue
    try:
        with open(path, encoding="utf-8") as source_file:
            masked = mask_non_code(source_file.read())
    except UnicodeDecodeError:
        continue
    for pattern in patterns:
        for match in pattern.finditer(masked):
            line = masked.count("\n", 0, match.start()) + 1
            print(f"thread detachment is forbidden in {path}:{line}")
            failed = True
if failed:
    sys.exit(1)
'

sh "$repo_root/tools/check-library-sync-construction.sh" "$repo_root"
sh "$repo_root/tools/check-library-sync-ui-boundary.sh" "$repo_root"

catalogdb_sources="src/catalog/CatalogDb.cpp src/catalog/CatalogDbSchema.cpp src/catalog/CatalogDbSchemaOps.cpp src/catalog/CatalogDbWrite.cpp src/catalog/CatalogDbQuery.cpp src/catalog/CatalogDbSyncState.cpp src/catalog/CatalogDbHierarchy.cpp src/catalog/CatalogDbInternal.hpp src/catalog/CatalogDb.hpp"
catalogdb_prohibited='JellyfinApi|DownloadStore|LibraryCache|TitleOrganization|MovieTitle|movieOrganizationalLess|organizationalLess|LibrarySnapshot|seedLibrarySnapshot|readLibrarySnapshot|\.\./net/|\.\./download/|\.\./ui/|\.\./app/|\.\./cache/|\.\./library/'

for source in $catalogdb_sources; do
    if matches=$(grep -nE "$catalogdb_prohibited" "$source"); then
        printf '%s\n' "CatalogDb dependency boundary violation in $source:"
        printf '%s\n' "$matches"
        exit 1
    fi
done

# Test-only translation units must be registered in the test build and must
# never leak into the cross (device) build or the production source list.
test_only_sources="src/catalog/CatalogDbTestCommands.cpp"
for source in $test_only_sources; do
    relative=${source#src/}
    matches=$(grep -n "\$(SRC_DIR)/$relative" sources.mk || true)
    if [ -z "$matches" ]; then
        echo "missing test registration: $source"
        exit 1
    fi
    if ! printf '%s\n' "$matches" | grep -q "MIYOOFIN_TEST_SRCS"; then
        echo "test-only source leaked into production sources: $source"
        exit 1
    fi
    if grep -q "$source" Makefile.cross; then
        echo "test-only source leaked into cross build: $source"
        exit 1
    fi
done

# Moved modules must not be referenced from their old locations.
stale_paths='\.\./app/UiDiagnostics\.hpp|\.\./ui/MovieTitle\.hpp|\.\./ui/TitleOrganization\.hpp|\.\./download/HlsPlaylist\.hpp|\.\./download/HlsProfile\.hpp|\.\./catalog/CatalogPrimitives\.hpp'
if matches=$(grep -rnE "$stale_paths" src/ tests/ Makefile Makefile.cross); then
    printf '%s\n' "stale module include path(s) found:"
    printf '%s\n' "$matches"
    exit 1
fi

required_sources="CatalogDbSchema.cpp CatalogDbSchemaOps.cpp CatalogDbWrite.cpp CatalogDbQuery.cpp CatalogDbSyncState.cpp CatalogDbHierarchy.cpp LibrarySyncIncremental.cpp LibrarySyncEvents.cpp AppSession.cpp AppPlayback.cpp PerformanceTelemetryRecord.cpp PerformanceTelemetryService.cpp PerformanceTelemetrySnapshot.cpp SeriesScreenWorker.cpp SeriesScreenNavigation.cpp SeriesScreenRender.cpp MovieDetailsWorker.cpp MovieDetailsRender.cpp EpisodeBrowserData.cpp HomeScreenOffline.cpp HomeScreenSyncApply.cpp JellyfinLibraryEventParse.cpp JellyfinLibraryEventQueue.cpp JellyfinLibraryEventSocket.cpp"
for source in $required_sources; do
    grep -q "$source" sources.mk || { echo "missing host/test registration: $source"; exit 1; }
done

# Cross build must consume the shared source list so every required module is
# compiled for the device.
grep -q 'include sources\.mk' Makefile.cross || { echo "Makefile.cross does not include sources.mk"; exit 1; }

for source in src/ui/screens/*Render.cpp; do
    if matches=$(grep -nE 'HttpClient|RouteRequest|JellyfinApi|ArtworkUrl|curl/curl' "$source"); then
        printf '%s\n' "render dependency boundary violation in $source:"
        printf '%s\n' "$matches"
        exit 1
    fi
done

make test
git diff --check
