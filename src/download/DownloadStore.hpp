#ifndef MIYOOFIN_DOWNLOAD_STORE_HPP
#define MIYOOFIN_DOWNLOAD_STORE_HPP

#include "DownloadTypes.hpp"
#include <string>
#include <vector>

namespace miyoofin {
class DownloadStore {
public:
    explicit DownloadStore(std::string root="downloads") : m_root(std::move(root)) {}
    const std::string &root() const { return m_root; }
    static std::string scopeKey(const std::string &serverUrl, const std::string &userId);
    std::string scopePath(const std::string &scope) const;
    std::string itemPath(const std::string &scope, const std::string &itemId) const;
    std::string manifestPath(const std::string &scope, const std::string &itemId) const;
    std::string chunkPath(const std::string &scope, const std::string &itemId, std::uint64_t index, bool part=false) const;
    std::string segmentPath(const std::string &scope, const std::string &itemId, std::uint64_t index, bool part=false) const;
    bool ensureHlsDirectories(const std::string &scope, const std::string &itemId) const;
    bool isCompleteSegment(const std::string &scope, const std::string &itemId, std::uint64_t index) const;
    std::uint64_t firstIncompleteSegment(const std::string &scope, const DownloadItem &item) const;
    bool saveManifest(const std::string &scope, const DownloadItem &item, std::string *error=nullptr) const;
    bool loadManifest(const std::string &scope, const std::string &itemId, DownloadItem &item, std::string *error=nullptr) const;
    bool saveIndex(const std::string &scope, const std::vector<DownloadItem> &items, std::string *error=nullptr) const;
    bool loadIndex(const std::string &scope, std::vector<DownloadItem> &items, std::string *error=nullptr) const;
    bool rebuildIndex(const std::string &scope, std::vector<DownloadItem> &items, std::string *error=nullptr) const;
    bool reconcile(const std::string &scope, DownloadItem &item, std::string *error=nullptr) const;
    bool validateCompletedDownload(const std::string &scope, const DownloadItem &item, std::string *error=nullptr) const;
    bool removePartialBytes(const std::string &scope, const std::string &itemId, std::string *error=nullptr) const;
    bool removeItem(const std::string &scope, const std::string &itemId, std::string *error=nullptr) const;
private: std::string m_root;
};
}
#endif
