#ifndef MIYOOFIN_SYNC_STATE_HPP
#define MIYOOFIN_SYNC_STATE_HPP

#include <cstdint>
#include <string>
namespace miyoofin {
// Deliberately small, scoped and durable.  This is a completion checkpoint,
// not UI state: progress is never written here.
struct SyncState { std::int64_t lastSuccessfulMs=0, lastReconcileMs=0; };
class SyncStateStore {
public:
    static std::string path(const std::string &root, const std::string &scope);
    static bool load(const std::string &path, SyncState &state, std::string *error=nullptr);
    static bool save(const std::string &path, const SyncState &state, std::string *error=nullptr);
};
inline bool syncStateFresh(const SyncState &s, std::int64_t nowMs, std::int64_t freshMs) {
    return s.lastSuccessfulMs > 0 && nowMs >= s.lastSuccessfulMs && nowMs-s.lastSuccessfulMs < freshMs;
}
}
#endif
