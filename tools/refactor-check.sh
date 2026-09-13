#!/bin/sh
set -eu

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
repo_root=$(CDPATH= cd "$script_dir/.." && pwd)
cd "$repo_root"

catalogdb_sources="src/catalog/CatalogDb.cpp src/catalog/CatalogDbSchema.cpp src/catalog/CatalogDbWrite.cpp src/catalog/CatalogDbQuery.cpp src/catalog/CatalogDbSyncState.cpp src/catalog/CatalogDbHierarchy.cpp src/catalog/CatalogDbInternal.hpp src/catalog/CatalogDb.hpp"
catalogdb_prohibited='JellyfinApi|DownloadStore|LibraryCache|TitleOrganization|MovieTitle|movieOrganizationalLess|organizationalLess|LibrarySnapshot|seedLibrarySnapshot|readLibrarySnapshot|\.\./net/|\.\./download/|\.\./ui/'

for source in $catalogdb_sources; do
    if matches=$(grep -nE "$catalogdb_prohibited" "$source"); then
        printf '%s\n' "CatalogDb dependency boundary violation in $source:"
        printf '%s\n' "$matches"
        exit 1
    fi
done

required_sources="CatalogDbSchema.cpp CatalogDbWrite.cpp CatalogDbQuery.cpp CatalogDbSyncState.cpp CatalogDbHierarchy.cpp LibrarySyncIncremental.cpp LibrarySyncEvents.cpp AppSession.cpp AppPlayback.cpp PerformanceTelemetryRecord.cpp PerformanceTelemetryService.cpp PerformanceTelemetrySnapshot.cpp SeriesScreenWorker.cpp SeriesScreenNavigation.cpp SeriesScreenRender.cpp MovieDetailsWorker.cpp MovieDetailsRender.cpp EpisodeBrowserData.cpp HomeScreenOffline.cpp HomeScreenSyncApply.cpp"
for source in $required_sources; do
    grep -q "$source" Makefile || { echo "missing host/test registration: $source"; exit 1; }
    grep -q "$source" Makefile.cross || { echo "missing cross registration: $source"; exit 1; }
done

for source in src/ui/screens/MovieDetailsRender.cpp; do
    if grep -nE 'HttpClient|RouteRequest|JellyfinApi|ArtworkUrl|curl/curl' "$source"; then
        echo "render dependency boundary violation in $source"
        exit 1
    fi
done

make test
git diff --check
