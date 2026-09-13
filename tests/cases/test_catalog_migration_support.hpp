#include <cerrno>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../../vendor/sqlite/sqlite3.h"
#include "../../src/catalog/CatalogPrimitives.hpp"

namespace {

struct CatalogMigrationTestPaths {
    std::string scope;
    std::string libraryDirectory;
    std::string offlineDirectory;
    std::string finalPath;
    std::string migratingPath;
    std::string legacyPath;
};

CatalogMigrationTestPaths catalogMigrationTestPaths(
    const std::string &url, const std::string &user)
{
    CatalogMigrationTestPaths paths;
    paths.scope = LibraryCache::scopeKey(url, user);
    CHECK(paths.scope == miyoofin::catalog::scopeKey(url, user));
    paths.libraryDirectory = "cache/library/" + paths.scope;
    paths.offlineDirectory = "cache/offline/" + paths.scope;
    paths.finalPath = paths.libraryDirectory + "/catalog.sqlite3";
    paths.migratingPath = paths.finalPath + ".migrating";
    CHECK(paths.finalPath
          == miyoofin::catalog::catalogPath("cache", paths.scope));
    CHECK(paths.migratingPath
          == miyoofin::catalog::migratingCatalogPath("cache", paths.scope));
    paths.legacyPath = paths.offlineDirectory + "/catalog.v1";
    return paths;
}

void removeCatalogMigrationTestPaths(const CatalogMigrationTestPaths &paths)
{
    const std::string files[] = {
        paths.finalPath, paths.finalPath + "-journal",
        paths.finalPath + "-wal", paths.finalPath + "-shm",
        paths.migratingPath, paths.migratingPath + "-journal",
        paths.migratingPath + "-wal", paths.migratingPath + "-shm",
        paths.legacyPath};
    for (const auto &file : files) {
        std::remove(file.c_str());
    }
    std::remove(paths.libraryDirectory.c_str());
    std::remove(paths.offlineDirectory.c_str());
    ::rmdir(paths.libraryDirectory.c_str());
    ::rmdir(paths.offlineDirectory.c_str());
}

bool makeCatalogMigrationTestDirectory(const std::string &path)
{
    for (std::size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            const std::string directory = path.substr(0, i);
            if (!directory.empty() && ::mkdir(directory.c_str(), 0755)
                && errno != EEXIST) {
                return false;
            }
        }
    }
    return true;
}

void writeCatalogMigrationFile(const std::string &path, const std::string &value)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

} // namespace


