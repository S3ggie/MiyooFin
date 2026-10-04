#ifndef MIYOOFIN_DOWNLOAD_STORE_HPP
#define MIYOOFIN_DOWNLOAD_STORE_HPP

#include "DownloadTypes.hpp"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace miyoofin {
/// Outcome of reading a scope's whole library (see DownloadStore::readLibrary).
enum class LibraryStatus
{
    Loaded,     ///< the index and every manifest it lists were read
    NewScope,   ///< storage is readable and holds nothing for this scope: a genuinely empty library
    Rebuilt,    ///< the index was missing/corrupt and the manifests were all scanned successfully
    Unavailable ///< storage (or part of it) could not be read: NOT an empty library
};
/// Deletes <itemDir>/playback-tracks.txt and <itemDir>/subs/ (offline subtitle sidecars).
void removeSubtitleSidecars(const std::string& itemDir);

class DownloadStore
{
  public:
    explicit DownloadStore(std::string root = "downloads") : m_root(std::move(root)) {}
    const std::string& root() const
    {
        return m_root;
    }
    static std::string scopeKey(const std::string& serverUrl, const std::string& userId);
    std::string scopePath(const std::string& scope) const;
    std::string itemPath(const std::string& scope, const std::string& itemId) const;
    std::string manifestPath(const std::string& scope, const std::string& itemId) const;
    std::string chunkPath(const std::string& scope, const std::string& itemId, std::uint64_t index,
                          bool part = false) const;
    std::string segmentPath(const std::string& scope, const std::string& itemId,
                            std::uint64_t index, bool part = false) const;
    bool ensureHlsDirectories(const std::string& scope, const std::string& itemId) const;
    /// Structural MPEG-TS validation of a segment file (see MpegTsValidator.hpp for what that does
    /// and does not guarantee). `full` checks every packet (used once, on a freshly downloaded
    /// segment); otherwise the opening packets plus a sample (used whenever the library is
    /// checked).
    static bool validHlsSegment(const std::string& path, std::uint64_t size, bool full,
                                std::string* why = nullptr);
    bool isCompleteSegment(const std::string& scope, const std::string& itemId,
                           std::uint64_t index) const;
    std::uint64_t firstIncompleteSegment(const std::string& scope, const DownloadItem& item) const;
    /// The exact bytes saveManifest() would write for `item` (lets a caller check later whether the
    /// item changed since it was written).
    static std::string manifestText(const DownloadItem& item);
    bool saveManifest(const std::string& scope, const DownloadItem& item,
                      std::string* error = nullptr) const;
    bool loadManifest(const std::string& scope, const std::string& itemId, DownloadItem& item,
                      std::string* error = nullptr) const;
    bool saveIndex(const std::string& scope, const std::vector<DownloadItem>& items,
                   std::string* error = nullptr) const;
    /// Creates the index only if none exists (O_EXCL; hard links and no-replace renames are not
    /// available on the device's FAT card). Exists means an index is already there and nothing was
    /// written. A crash between the create and the write leaves a header-less file, which
    /// readLibrary() treats as a damaged index and rebuilds from the manifests.
    enum class IndexCreate
    {
        Created,
        Exists,
        Failed
    };
    IndexCreate createIndexExclusive(const std::string& scope,
                                     const std::vector<DownloadItem>& items,
                                     std::string* error = nullptr) const;
    bool loadIndex(const std::string& scope, std::vector<DownloadItem>& items,
                   std::string* error = nullptr) const;
    /// Read the durable metadata entries that represent locally available
    /// downloads. This never scans or modifies media bytes.
    bool loadCompleteMetadata(const std::string& scope, std::vector<DownloadItem>& items,
                              std::string* error = nullptr) const;
    /// Reads the library without modifying anything. Unlike loadIndex()/rebuildIndex() it keeps
    /// "nothing is there" apart from "could not look": a read error, a missing/non-directory
    /// storage root, or an unreadable manifest is Unavailable and `items` is left untouched, so a
    /// caller can never mistake a failed read for an empty library.
    LibraryStatus readLibrary(const std::string& scope, std::vector<DownloadItem>& items,
                              std::string* error = nullptr) const;
    /// The manifests under `scope`/items, ignoring the index (what readLibrary falls back to).
    LibraryStatus scanManifests(const std::string& scope, std::vector<DownloadItem>& items,
                                std::string* error = nullptr) const;
    /// True when the storage root can be used: it is a readable directory on the device it was
    /// established on, or it never held a library (no marker beside it) and does not exist yet
    /// while its parent does. False for a root that is a file, an unreadable one, a dangling
    /// link, a mount point whose storage is gone (different device than recorded), a missing root
    /// that a marker says held downloads, or one whose parent is gone.
    bool storageReadable() const;
    /// Where the "this storage held a library" marker lives: beside the root, not inside it.
    std::string markerPath() const;
    bool rebuildIndex(const std::string& scope, std::vector<DownloadItem>& items,
                      std::string* error = nullptr) const;
    /// reconcile() without the manifest write (see the .cpp).
    bool reconcileInMemory(const std::string& scope, DownloadItem& item,
                           std::string* error = nullptr) const;
    bool reconcile(const std::string& scope, DownloadItem& item,
                   std::string* error = nullptr) const;
    bool validateCompletedDownload(const std::string& scope, const DownloadItem& item,
                                   std::string* error = nullptr) const;
    bool removePartialBytes(const std::string& scope, const std::string& itemId,
                            std::string* error = nullptr) const;
    bool removeItem(const std::string& scope, const std::string& itemId,
                    std::string* error = nullptr) const;

  private:
    /// Records (once per store, after the first successful write) that this root has held
    /// downloads, together with the device it was on, so a later absent or swapped root is not
    /// mistaken for a first run.
    void noteEstablished() const;
    std::string m_root;
    std::shared_ptr<std::atomic<bool>> m_established = std::make_shared<std::atomic<bool>>(false);
};
}
#endif
