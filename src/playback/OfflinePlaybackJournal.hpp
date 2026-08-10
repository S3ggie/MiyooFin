#ifndef MIYOOFIN_OFFLINE_PLAYBACK_JOURNAL_HPP
#define MIYOOFIN_OFFLINE_PLAYBACK_JOURNAL_HPP
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
namespace miyoofin {
struct OfflinePlaybackEntry { std::string itemId,itemType; std::int64_t baseServerTicks=0,finalTicks=0; std::uint64_t localTimestamp=0; bool conflict=false, serverMissing=false; };
enum class OfflineSyncDecision { Push, Conflict, Retry };
inline OfflineSyncDecision decideOfflineSync(std::int64_t base,std::int64_t server,bool fetched){return !fetched?OfflineSyncDecision::Retry:(server==base?OfflineSyncDecision::Push:OfflineSyncDecision::Conflict);}
enum class OfflineJournalRequestStatus { Success, Transient, Unauthorized, Missing };
struct OfflineJournalSyncStats { bool changed=false, retry=false, unauthorized=false; unsigned pushed=0, conflicts=0; };
/// Pure reconciliation step. Network callbacks make this deterministic to test;
/// callers persist entries only when stats.changed is true.
OfflineJournalSyncStats syncOfflinePlaybackEntries(
    std::vector<OfflinePlaybackEntry> &entries,
    const std::function<OfflineJournalRequestStatus(const OfflinePlaybackEntry &, std::int64_t &)> &fetch,
    const std::function<OfflineJournalRequestStatus(const OfflinePlaybackEntry &)> &submit);
class OfflinePlaybackJournal { public:
 static std::string path(const std::string&root,const std::string&scope);
 static bool load(const std::string&,std::vector<OfflinePlaybackEntry>&,std::string * =nullptr);
 static bool save(const std::string&,const std::vector<OfflinePlaybackEntry>&,std::string * =nullptr);
 static bool upsert(const std::string&,const OfflinePlaybackEntry&,std::string * =nullptr);
 /// Removes only an entry already confirmed permanently missing by the server.
 static bool discardMissing(const std::string&,const std::string&,std::string * =nullptr);
}; }
#endif
