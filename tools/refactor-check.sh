#!/bin/sh
set -eu

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
repo_root=$(CDPATH= cd "$script_dir/.." && pwd)
cd "$repo_root"

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
