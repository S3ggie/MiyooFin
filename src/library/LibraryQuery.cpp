#include "LibraryQuery.hpp"
#include "../catalog/CatalogDb.hpp"

namespace miyoofin::library {

namespace {

LibraryQueryErrorCategory toLibraryError(CatalogDbErrorCategory error)
{
    switch (error) {
    case CatalogDbErrorCategory::None:
        return LibraryQueryErrorCategory::None;
    case CatalogDbErrorCategory::InvalidIdentity:
        return LibraryQueryErrorCategory::InvalidIdentity;
    case CatalogDbErrorCategory::ScopeNotReady:
        return LibraryQueryErrorCategory::ScopeNotReady;
    case CatalogDbErrorCategory::OpenFailed:
        return LibraryQueryErrorCategory::OpenFailed;
    case CatalogDbErrorCategory::WrongApplicationId:
        return LibraryQueryErrorCategory::WrongApplicationId;
    case CatalogDbErrorCategory::UnsupportedVersion:
        return LibraryQueryErrorCategory::UnsupportedVersion;
    case CatalogDbErrorCategory::CorruptOrIo:
        return LibraryQueryErrorCategory::CorruptOrIo;
    case CatalogDbErrorCategory::ConfigurationFailed:
        return LibraryQueryErrorCategory::ConfigurationFailed;
    case CatalogDbErrorCategory::SqliteError:
        return LibraryQueryErrorCategory::SqliteError;
    case CatalogDbErrorCategory::Superseded:
        return LibraryQueryErrorCategory::Superseded;
    }
    return LibraryQueryErrorCategory::SqliteError;
}

CatalogDbPageCursor toCatalogCursor(const LibraryPageCursor& cursor)
{
    CatalogDbPageCursor out;
    out.sortKey = cursor.sortKey;
    out.title = cursor.title;
    out.id = cursor.id;
    out.valid = cursor.valid;
    return out;
}

LibraryPageCursor toLibraryCursor(CatalogDbPageCursor cursor)
{
    LibraryPageCursor out;
    out.sortKey = std::move(cursor.sortKey);
    out.title = std::move(cursor.title);
    out.id = std::move(cursor.id);
    out.valid = cursor.valid;
    return out;
}

std::vector<LibraryMembership>
toLibraryMemberships(std::vector<CatalogDbMediaPageMembership> memberships)
{
    std::vector<LibraryMembership> out;
    out.reserve(memberships.size());
    for (auto& membership : memberships) {
        LibraryMembership converted;
        converted.viewId = std::move(membership.viewId);
        converted.viewName = std::move(membership.viewName);
        converted.collectionType = std::move(membership.collectionType);
        out.push_back(std::move(converted));
    }
    return out;
}

CatalogDbJobMetadata metadataFor(std::uint64_t scopeEpoch,
                                 const std::shared_ptr<std::atomic_bool>& cancellation)
{
    CatalogDbJobMetadata metadata;
    metadata.scopeEpoch = scopeEpoch;
    metadata.cancellation = cancellation;
    return metadata;
}

MediaPage toLibraryPage(CatalogDbMediaPageResult result)
{
    MediaPage out;
    out.success = result.success;
    out.cancelled = result.cancelled;
    out.superseded = result.superseded;
    out.error = toLibraryError(result.error);
    out.message = std::move(result.message);
    out.hasMore = result.hasMore;
    out.items = std::move(result.items);
    for (auto& entry : result.membershipsByItem)
        out.membershipsByItem.emplace(std::move(entry.first),
                                      toLibraryMemberships(std::move(entry.second)));
    out.next = toLibraryCursor(std::move(result.next));
    return out;
}

HierarchyPage toLibraryHierarchyPage(CatalogDbHierarchyResult result)
{
    HierarchyPage out;
    out.success = result.success;
    out.cancelled = result.cancelled;
    out.superseded = result.superseded;
    out.error = toLibraryError(result.error);
    out.message = std::move(result.message);
    out.items = std::move(result.items);
    return out;
}

// Structured result for a continuation that was never dispatched. Mirrors
// CatalogDb's own stopped/full command results so callers keep the same
// non-throwing, non-success shape as an ordinary failed read. Queue-full maps
// to OpenFailed exactly as CatalogDb's RejectedFull does.
MediaPage rejectedMediaPage(LibraryQuery::ContinuationSubmitResult result)
{
    MediaPage page;
    if (result == LibraryQuery::ContinuationSubmitResult::QueueFull) {
        page.error = LibraryQueryErrorCategory::OpenFailed;
        page.message = "LibraryQuery continuation queue is full";
    } else {
        page.error = LibraryQueryErrorCategory::ScopeNotReady;
        page.message = "LibraryQuery stopped before page read";
    }
    return page;
}

HierarchyPage rejectedHierarchyPage(LibraryQuery::ContinuationSubmitResult result)
{
    HierarchyPage page;
    if (result == LibraryQuery::ContinuationSubmitResult::QueueFull) {
        page.error = LibraryQueryErrorCategory::OpenFailed;
        page.message = "LibraryQuery continuation queue is full";
    } else {
        page.error = LibraryQueryErrorCategory::ScopeNotReady;
        page.message = "LibraryQuery stopped before hierarchy read";
    }
    return page;
}

} // namespace

LibraryQuery::LibraryQuery(std::shared_ptr<CatalogDb> db, std::uint64_t epoch)
    : m_db(std::move(db)), m_scopeEpoch(epoch)
{
    // Start after every member is initialized so the worker can never observe
    // a partially constructed object. The worker only touches the
    // continuation queue until a job is submitted.
    m_continuationThread = std::thread(&LibraryQuery::continuationLoop, this);
}

LibraryQuery::~LibraryQuery()
{
    {
        std::lock_guard<std::mutex> lock(m_continuationMutex);
        m_continuationStopping = true;
        for (auto& continuation : m_continuations) {
            // The continuation was never dispatched, so its promise is still
            // unset; fulfill it here instead of destroying it and producing a
            // broken_promise for a caller that still holds the future.
            continuation.stop();
        }
        m_continuations.clear();
    }
    m_continuationWake.notify_all();
    if (m_continuationThread.joinable()) {
        m_continuationThread.join();
    }
}

LibraryQuery::ContinuationSubmitResult LibraryQuery::submitContinuation(Continuation continuation)
{
    {
        std::lock_guard<std::mutex> lock(m_continuationMutex);
        if (m_continuationStopping) {
            // Shutdown won the race: report it so the caller completes the
            // promise directly rather than enqueueing work the stopped worker
            // will never run.
            return ContinuationSubmitResult::Stopped;
        }
        if (m_continuations.size() >= kMaxPendingContinuations) {
            // Bounded queue: shed the submission instead of blocking the
            // caller or growing without bound.
            return ContinuationSubmitResult::QueueFull;
        }
        m_continuations.push_back(std::move(continuation));
        m_continuationWake.notify_one();
    }
    return ContinuationSubmitResult::Accepted;
}

void LibraryQuery::continuationLoop()
{
    for (;;) {
        Continuation continuation;
        {
            std::unique_lock<std::mutex> lock(m_continuationMutex);
            m_continuationWake.wait(
                lock, [this] { return m_continuationStopping || !m_continuations.empty(); });
            if (m_continuationStopping && m_continuations.empty()) {
                return;
            }
            continuation = std::move(m_continuations.front());
            m_continuations.pop_front();
        }
        continuation.run();
    }
}

std::future<MediaPage> LibraryQuery::movies(int letter, std::size_t limit,
                                            const LibraryPageCursor& after,
                                            const std::shared_ptr<std::atomic_bool>& cancellation)
{
    auto catalogFuture = std::make_shared<std::future<CatalogDbMediaPageResult>>(
        m_db->readMediaPage("movie", letter, limit, toCatalogCursor(after),
                            metadataFor(m_scopeEpoch, cancellation)));
    auto promise = std::make_shared<std::promise<MediaPage>>();
    auto future = promise->get_future();
    const auto submitted = submitContinuation(
        {[catalogFuture, promise] {
             try {
                 promise->set_value(toLibraryPage(catalogFuture->get()));
             } catch (...) {
                 promise->set_exception(std::current_exception());
             }
         },
         [promise] { promise->set_value(rejectedMediaPage(ContinuationSubmitResult::Stopped)); }});
    if (submitted != ContinuationSubmitResult::Accepted)
        promise->set_value(rejectedMediaPage(submitted));
    return future;
}

std::future<MediaPage> LibraryQuery::shows(int letter, std::size_t limit,
                                           const LibraryPageCursor& after,
                                           const std::shared_ptr<std::atomic_bool>& cancellation)
{
    auto catalogFuture =
        std::make_shared<std::future<CatalogDbMediaPageResult>>(m_db->readMediaPage(
            "show", letter, limit, toCatalogCursor(after), metadataFor(m_scopeEpoch, cancellation),
            CatalogDbMediaPageFilter::Supported));
    auto promise = std::make_shared<std::promise<MediaPage>>();
    auto future = promise->get_future();
    const auto submitted = submitContinuation(
        {[catalogFuture, promise] {
             try {
                 promise->set_value(toLibraryPage(catalogFuture->get()));
             } catch (...) {
                 promise->set_exception(std::current_exception());
             }
         },
         [promise] { promise->set_value(rejectedMediaPage(ContinuationSubmitResult::Stopped)); }});
    if (submitted != ContinuationSubmitResult::Accepted)
        promise->set_value(rejectedMediaPage(submitted));
    return future;
}

bool LibraryQuery::scopeReady() const
{
    if (!m_db || m_scopeEpoch == 0)
        return false;
    const CatalogDbScopeState state = m_db->scopeState();
    return state.requestedEpoch == m_scopeEpoch && state.ready;
}

std::future<MediaPage> LibraryQuery::anime(int letter, std::size_t limit,
                                           const LibraryPageCursor& after)
{
    auto catalogFuture = std::make_shared<std::future<CatalogDbMediaPageResult>>(
        m_db->readMediaPage("show", letter, limit, toCatalogCursor(after),
                            metadataFor(m_scopeEpoch, {}), CatalogDbMediaPageFilter::Anime));
    auto promise = std::make_shared<std::promise<MediaPage>>();
    auto future = promise->get_future();
    const auto submitted = submitContinuation(
        {[catalogFuture, promise] {
             try {
                 promise->set_value(toLibraryPage(catalogFuture->get()));
             } catch (...) {
                 promise->set_exception(std::current_exception());
             }
         },
         [promise] { promise->set_value(rejectedMediaPage(ContinuationSubmitResult::Stopped)); }});
    if (submitted != ContinuationSubmitResult::Accepted)
        promise->set_value(rejectedMediaPage(submitted));
    return future;
}

std::future<HierarchyPage>
LibraryQuery::seasons(const std::string& id, const std::shared_ptr<std::atomic_bool>& cancellation)
{
    auto catalogFuture = std::make_shared<std::future<CatalogDbHierarchyResult>>(
        m_db->getSeasons(id, metadataFor(m_scopeEpoch, cancellation)));
    auto promise = std::make_shared<std::promise<HierarchyPage>>();
    auto future = promise->get_future();
    const auto submitted = submitContinuation(
        {[catalogFuture, promise] {
             try {
                 promise->set_value(toLibraryHierarchyPage(catalogFuture->get()));
             } catch (...) {
                 promise->set_exception(std::current_exception());
             }
         },
         [promise] {
             promise->set_value(rejectedHierarchyPage(ContinuationSubmitResult::Stopped));
         }});
    if (submitted != ContinuationSubmitResult::Accepted)
        promise->set_value(rejectedHierarchyPage(submitted));
    return future;
}
std::future<HierarchyPage>
LibraryQuery::episodes(const std::string& id, const std::shared_ptr<std::atomic_bool>& cancellation)
{
    auto catalogFuture = std::make_shared<std::future<CatalogDbHierarchyResult>>(
        m_db->getEpisodes(id, metadataFor(m_scopeEpoch, cancellation)));
    auto promise = std::make_shared<std::promise<HierarchyPage>>();
    auto future = promise->get_future();
    const auto submitted = submitContinuation(
        {[catalogFuture, promise] {
             try {
                 promise->set_value(toLibraryHierarchyPage(catalogFuture->get()));
             } catch (...) {
                 promise->set_exception(std::current_exception());
             }
         },
         [promise] {
             promise->set_value(rejectedHierarchyPage(ContinuationSubmitResult::Stopped));
         }});
    if (submitted != ContinuationSubmitResult::Accepted)
        promise->set_value(rejectedHierarchyPage(submitted));
    return future;
}
std::future<HierarchyPage>
LibraryQuery::itemsByIds(const std::vector<std::string>& itemIds,
                         const std::shared_ptr<std::atomic_bool>& cancellation)
{
    auto catalogFuture = std::make_shared<std::future<CatalogDbHierarchyResult>>(
        m_db->readMediaItemsByIds(itemIds, metadataFor(m_scopeEpoch, cancellation)));
    auto promise = std::make_shared<std::promise<HierarchyPage>>();
    auto future = promise->get_future();
    const auto submitted = submitContinuation(
        {[catalogFuture, promise] {
             try {
                 promise->set_value(toLibraryHierarchyPage(catalogFuture->get()));
             } catch (...) {
                 promise->set_exception(std::current_exception());
             }
         },
         [promise] {
             promise->set_value(rejectedHierarchyPage(ContinuationSubmitResult::Stopped));
         }});
    if (submitted != ContinuationSubmitResult::Accepted)
        promise->set_value(rejectedHierarchyPage(submitted));
    return future;
}
}
