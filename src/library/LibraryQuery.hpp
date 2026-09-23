#ifndef MIYOOFIN_LIBRARY_QUERY_HPP
#define MIYOOFIN_LIBRARY_QUERY_HPP

#include "../data/MediaItem.hpp"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace miyoofin {

class CatalogDb;

namespace library {

class LibraryCoordinator;

enum class LibraryQueryErrorCategory : unsigned char
{
    None,
    InvalidIdentity,
    ScopeNotReady,
    OpenFailed,
    WrongApplicationId,
    UnsupportedVersion,
    CorruptOrIo,
    ConfigurationFailed,
    SqliteError,
    Superseded,
};

struct LibraryPageCursor
{
    std::string sortKey;
    std::string title;
    std::string id;
    bool valid = false;
};

struct LibraryMembership
{
    std::string viewId;
    std::string viewName;
    std::string collectionType;
};

struct MediaPage
{
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    LibraryQueryErrorCategory error = LibraryQueryErrorCategory::None;
    std::string message;
    bool hasMore = false;
    std::vector<MediaItem> items;
    std::map<std::string, std::vector<LibraryMembership>> membershipsByItem;
    LibraryPageCursor next;
};

struct HierarchyPage
{
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    LibraryQueryErrorCategory error = LibraryQueryErrorCategory::None;
    std::string message;
    std::vector<MediaItem> items;
};

class LibraryQuery
{
  public:
    // Bounded depth of the single continuation queue. The value is deliberately
    // small and mirrors CatalogDb's pending-job cap (CatalogDb::kMaxPendingJobs):
    // a caller that outpaces the one conversion worker is shed with a structured
    // queue-full result instead of letting the queue grow without bound.
    static constexpr std::size_t kMaxPendingContinuations = 16;

    // Outcome of handing a continuation to the executor. Exposed so the
    // type-specific result builders can describe a rejected submission.
    enum class ContinuationSubmitResult : unsigned char
    {
        Accepted,
        Stopped,
        QueueFull,
    };

    ~LibraryQuery();

    std::future<MediaPage> movies(int alphabetLetter, std::size_t limit,
                                  const LibraryPageCursor& after = {},
                                  const std::shared_ptr<std::atomic_bool>& cancellation = {});
    std::future<MediaPage> shows(int alphabetLetter, std::size_t limit,
                                 const LibraryPageCursor& after = {},
                                 const std::shared_ptr<std::atomic_bool>& cancellation = {});
    std::future<MediaPage> anime(int alphabetLetter, std::size_t limit,
                                 const LibraryPageCursor& after = {});
    std::future<HierarchyPage> seasons(const std::string& seriesId,
                                       const std::shared_ptr<std::atomic_bool>& cancellation = {});
    std::future<HierarchyPage> episodes(const std::string& seasonId,
                                        const std::shared_ptr<std::atomic_bool>& cancellation = {});
    std::future<HierarchyPage>
    itemsByIds(const std::vector<std::string>& itemIds,
               const std::shared_ptr<std::atomic_bool>& cancellation = {});
    std::uint64_t scopeEpoch() const
    {
        return m_scopeEpoch;
    }
    bool scopeReady() const;

  private:
    friend class LibraryCoordinator;

    LibraryQuery(std::shared_ptr<CatalogDb> db, std::uint64_t scopeEpoch);

    // A single bounded FIFO continuation executor shared by every query method.
    // Each call hands the executor one continuation that waits on the CatalogDb
    // worker's future and publishes the converted domain result; `stop`
    // publishes a structured stopped result instead when the executor is
    // shutting down or abandons a queued continuation, so no caller observes a
    // broken promise. One worker serializes conversions, so no per-query thread
    // is created and conversion never runs on the caller/UI thread.
    //
    // The queue is capped at kMaxPendingContinuations. submitContinuation never
    // blocks the caller: an over-capacity submission is reported back so the
    // caller can complete its promise immediately with a structured
    // queue-full/unavailable result (mirroring CatalogDb's own RejectedFull
    // shape). Exactly one of run/stop or the submit result completes the
    // promise.
    //
    // Lifetime: the worker is joined in the destructor. The in-flight
    // continuation waits on a CatalogDb future, but LibraryQuery keeps the
    // CatalogDb alive through m_db, and CatalogDb always completes or fulfills
    // its futures while alive (including when stopping), so the join cannot
    // deadlock. The one exception is the test-only paused CatalogDb worker:
    // callers that pause it must resume it before destroying the query.
    struct Continuation
    {
        std::function<void()> run;
        std::function<void()> stop;
    };

    ContinuationSubmitResult submitContinuation(Continuation continuation);
    void continuationLoop();

    std::shared_ptr<CatalogDb> m_db;
    std::uint64_t m_scopeEpoch;

    std::mutex m_continuationMutex;
    std::condition_variable m_continuationWake;
    std::deque<Continuation> m_continuations;
    std::thread m_continuationThread;
    bool m_continuationStopping = false;
};

}
}
#endif
