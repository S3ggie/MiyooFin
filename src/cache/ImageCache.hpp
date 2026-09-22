#ifndef MIYOOFIN_IMAGE_CACHE_HPP
#define MIYOOFIN_IMAGE_CACHE_HPP

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include "../net/ArtworkUrl.hpp"

namespace miyoofin {

/// Synchronous disk cache for JPEG artwork images.
///
/// B5a provides only infrastructure helpers — no worker threads,
/// no request queues, no LRU.  All methods are blocking and
/// filesystem-only.  A janitor runs periodically to enforce the
/// disk cap by evicting oldest files first.
class ImageCache
{
  public:
    // --- Disk cap constants (single source of truth) -----------------------
    /// Maximum bytes the images cache directory is allowed to occupy.
    static constexpr std::int64_t kMaxImageCacheBytes = 128LL * 1024 * 1024;
    /// After pruning, target this fraction of the cap to avoid thrashing.
    static constexpr double kImageCacheHysteresisRatio = 0.90;
    /// Skip files modified within the last N seconds (avoids racing writers).
    static constexpr std::int64_t kImageCacheStaleSec = 5;

    /// Lightweight file metadata for janitor victim selection.
    struct JanitorFileEntry
    {
        std::string path;
        std::int64_t size;  ///< File size in bytes.
        std::int64_t mtime; ///< Modification time (Unix epoch seconds).
    };

    /// Reorders entries (oldest mtime first) via std::stable_sort and
    /// selects files to delete until totalSize <= targetBytes.  Returns
    /// indices into the reordered vector.  Does NOT touch the filesystem.
    static std::vector<std::size_t> selectCacheVictims(std::vector<JanitorFileEntry>& entries,
                                                       std::int64_t targetBytes);

    /// Return the cache directory path (default: "cache/images/").
    static const std::string& cacheDir();

    /// Set the cache directory at runtime (for testing).
    static void setCacheDir(const std::string& dir);

    /// Build a deterministic cache filename for the given parameters.
    /// Format: {itemId}_{imageType}_{tag}_{width}x{height}.jpg
    static std::string cacheFilename(const std::string& itemId, ImageType type,
                                     const std::string& imageTag, int width, int height);

    /// Full filesystem path for a cached image.
    static std::string cachePath(const std::string& itemId, ImageType type,
                                 const std::string& imageTag, int width, int height);

    /// Check whether a cached JPEG exists on disk.
    static bool isCached(const std::string& itemId, ImageType type, const std::string& imageTag,
                         int width, int height);

    /// Read cached JPEG bytes.  Returns empty vector on miss.
    static std::vector<unsigned char> readCached(const std::string& itemId, ImageType type,
                                                 const std::string& imageTag, int width,
                                                 int height);

    /// Write JPEG bytes to the cache.  Creates directories as needed.
    /// Returns true on success.
    static bool writeToCache(const std::string& itemId, ImageType type, const std::string& imageTag,
                             int width, int height, const unsigned char* data, size_t size);
    /// Remove exactly one known cached image; never scans the cache directory.
    static bool removeCached(const std::string& itemId, ImageType type, const std::string& imageTag,
                             int width, int height);

    /// Scan the cache directory; if total size exceeds the cap, delete
    /// oldest-first (by mtime) until under the hysteresis target.
    /// Skips files modified within kImageCacheStaleSec seconds.
    /// Safe to call from a background worker thread.
    /// Returns the number of files deleted.
    static int runJanitor();
};

} // namespace miyoofin

#endif // MIYOOFIN_IMAGE_CACHE_HPP
