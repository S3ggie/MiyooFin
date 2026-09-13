#!/bin/sh
set -eu

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
repo_root=$(CDPATH= cd "$script_dir/.." && pwd)
cd "$repo_root"

catalogdb_sources="src/catalog/CatalogDb.cpp src/catalog/CatalogDbSchema.cpp src/catalog/CatalogDbInternal.hpp src/catalog/CatalogDb.hpp"
catalogdb_prohibited='JellyfinApi|DownloadStore|LibraryCache|TitleOrganization|MovieTitle|movieOrganizationalLess|organizationalLess|LibrarySnapshot|seedLibrarySnapshot|readLibrarySnapshot|\.\./net/|\.\./download/|\.\./ui/'

for source in $catalogdb_sources; do
    if matches=$(grep -nE "$catalogdb_prohibited" "$source"); then
        printf '%s\n' "CatalogDb dependency boundary violation in $source:"
        printf '%s\n' "$matches"
        exit 1
    fi
done

make test
git diff --check
