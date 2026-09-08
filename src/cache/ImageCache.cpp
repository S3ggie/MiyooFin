#include "ImageCache.hpp"
#include "../diagnostics/PerformanceTelemetry.hpp"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>

namespace miyoofin {

static std::string s_cacheDir = "cache/images/";

// -------------------------------------------------------------------
// Ensure a directory exists (recursive mkdir -p via parent).
// -------------------------------------------------------------------
static bool ensureDir(const std::string &path)
{
    // Try to create the directory; ignore EEXIST.
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

static bool ensureDirRecursive(const std::string &path)
{
    // Walk backwards to find the first existing parent, then create
    // each component forward.
    std::string partial;
    for (size_t i = 0; i < path.size(); ++i) {
        partial += path[i];
        if (path[i] == '/' || i == path.size() - 1) {
            // Don't try to create the root "/" or empty prefix
            if (partial.size() > 1) {
                ensureDir(partial);
            }
        }
    }
    // Final attempt
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// -------------------------------------------------------------------
// Public interface
// -------------------------------------------------------------------

const std::string &ImageCache::cacheDir()
{
    return s_cacheDir;
}

void ImageCache::setCacheDir(const std::string &dir)
{
    s_cacheDir = dir;
}

std::string ImageCache::cacheFilename(const std::string &itemId,
                                      ImageType type,
                                      const std::string &imageTag,
                                      int width,
                                      int height)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s_%s_%s_%dx%d.jpg",
                  itemId.c_str(),
                  imageTypeToString(type),
                  imageTag.c_str(),
                  width,
                  height);
    return std::string(buf);
}

std::string ImageCache::cachePath(const std::string &itemId,
                                  ImageType type,
                                  const std::string &imageTag,
                                  int width,
                                  int height)
{
    return s_cacheDir + cacheFilename(itemId, type, imageTag, width, height);
}

bool ImageCache::isCached(const std::string &itemId,
                          ImageType type,
                          const std::string &imageTag,
                          int width,
                          int height)
{
    std::string path = cachePath(itemId, type, imageTag, width, height);
    struct stat st;
    const bool hit = stat(path.c_str(), &st) == 0 && st.st_size > 0;
    performanceTelemetry().recordArtworkCacheProbe(hit);
    return hit;
}

std::vector<unsigned char> ImageCache::readCached(const std::string &itemId,
                                                  ImageType type,
                                                  const std::string &imageTag,
                                                  int width,
                                                  int height)
{
    std::string path = cachePath(itemId, type, imageTag, width, height);
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        performanceTelemetry().recordArtworkCacheRead(false, 0);
        return {};
    }

    std::fseek(f, 0, SEEK_END);
    long fileSize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    if (fileSize <= 0) {
        std::fclose(f);
        performanceTelemetry().recordArtworkCacheRead(false, 0);
        return {};
    }

    std::vector<unsigned char> data(static_cast<size_t>(fileSize));
    size_t bytesRead = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);

    if (bytesRead != data.size()) {
        performanceTelemetry().recordArtworkCacheRead(false, 0);
        return {};
    }

    performanceTelemetry().recordArtworkCacheRead(true, data.size());
    return data;
}

bool ImageCache::writeToCache(const std::string &itemId,
                              ImageType type,
                              const std::string &imageTag,
                              int width,
                              int height,
                              const unsigned char *data,
                              size_t size)
{
    if (!data || size == 0) {
        performanceTelemetry().recordArtworkCacheWrite(false, size);
        return false;
    }

    // Ensure the cache directory exists
    ensureDirRecursive(s_cacheDir);

    std::string path = cachePath(itemId, type, imageTag, width, height);
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        performanceTelemetry().recordArtworkCacheWrite(false, size);
        return false;
    }

    size_t written = std::fwrite(data, 1, size, f);
    std::fclose(f);

    const bool success = written == size;
    performanceTelemetry().recordArtworkCacheWrite(success, size);
    return success;
}

bool ImageCache::removeCached(const std::string &itemId, ImageType type,
                              const std::string &imageTag, int width, int height)
{
    std::string path = cachePath(itemId, type, imageTag, width, height);
    return std::remove(path.c_str()) == 0 || errno == ENOENT;
}

} // namespace miyoofin
