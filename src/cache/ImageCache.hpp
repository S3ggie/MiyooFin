#ifndef MIYOOFIN_IMAGE_CACHE_HPP
#define MIYOOFIN_IMAGE_CACHE_HPP

#include <string>
#include <vector>
#include "../net/ArtworkUrl.hpp"

namespace miyoofin {

/// Synchronous disk cache for JPEG artwork images.
///
/// B5a provides only infrastructure helpers — no worker threads,
/// no request queues, no LRU.  All methods are blocking and
/// filesystem-only.
class ImageCache {
public:
    /// Return the cache directory path (default: "cache/images/").
    static const std::string &cacheDir();

    /// Set the cache directory at runtime (for testing).
    static void setCacheDir(const std::string &dir);

    /// Build a deterministic cache filename for the given parameters.
    /// Format: {itemId}_{imageType}_{tag}_{width}x{height}.jpg
    static std::string cacheFilename(const std::string &itemId,
                                     ImageType type,
                                     const std::string &imageTag,
                                     int width,
                                     int height);

    /// Full filesystem path for a cached image.
    static std::string cachePath(const std::string &itemId,
                                 ImageType type,
                                 const std::string &imageTag,
                                 int width,
                                 int height);

    /// Check whether a cached JPEG exists on disk.
    static bool isCached(const std::string &itemId,
                         ImageType type,
                         const std::string &imageTag,
                         int width,
                         int height);

    /// Read cached JPEG bytes.  Returns empty vector on miss.
    static std::vector<unsigned char> readCached(const std::string &itemId,
                                                 ImageType type,
                                                 const std::string &imageTag,
                                                 int width,
                                                 int height);

    /// Write JPEG bytes to the cache.  Creates directories as needed.
    /// Returns true on success.
    static bool writeToCache(const std::string &itemId,
                             ImageType type,
                             const std::string &imageTag,
                             int width,
                             int height,
                             const unsigned char *data,
                             size_t size);
};

} // namespace miyoofin

#endif // MIYOOFIN_IMAGE_CACHE_HPP
