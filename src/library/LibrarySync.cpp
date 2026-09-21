#include "LibrarySync.hpp"
#include "OfflineLibraryQuery.hpp"
#include "../download/DownloadStore.hpp"
#include "../net/JellyfinApi.hpp"
#include "../net/JellyfinLibraryEvents.hpp"
#include "../net/RouteRequest.hpp"
#include <algorithm>
#include <cerrno>
#include <ctime>
#include <future>
#include <set>
#include <unistd.h>

namespace miyoofin {
namespace library {
namespace {

std::future<CatalogDbHierarchyWriteResult> rejectedHierarchyWrite(
    const char *message)
{
    std::promise<CatalogDbHierarchyWriteResult> promise;
    CatalogDbHierarchyWriteResult result;
    result.error = CatalogDbErrorCategory::ConfigurationFailed;
    result.message = message;
    promise.set_value(std::move(result));
    return promise.get_future();
}

}
library::LibrarySync::LibrarySync(Session session, std::shared_ptr<CatalogDb> db,
                         std::uint64_t scopeEpoch)
    : m_session(std::move(session)), m_db(std::move(db)),
      m_cancel(std::make_shared<std::atomic_bool>(false)),
      m_offlineGeneration(std::make_shared<std::atomic<std::uint64_t>>(0)) {
    m_metadata.scopeEpoch = scopeEpoch;
    m_metadata.cancellation = m_cancel;
}
}
library::LibrarySync::~LibrarySync()
{
    stop();
}
std::uint64_t library::LibrarySync::nextGeneration() { return ++m_generation; }
void library::LibrarySync::seedGeneration(std::uint64_t gen) {
    const auto cur = m_generation.load();
    if (gen > cur)
        m_generation.store(gen);
}
std::future<CatalogDbTopLevelSyncResult> library::LibrarySync::begin(std::uint64_t generation) {
    m_inFlight = true; m_success = false;
    return m_db->beginTopLevelSync(generation, m_metadata);
}
std::future<CatalogDbMediaPageUpsertResult> library::LibrarySync::stage(const CatalogDbMediaPageWrite &page) {
    return m_db->upsertMediaPage(page, m_metadata);
}
std::future<CatalogDbTopLevelSyncResult> library::LibrarySync::finalize(std::uint64_t generation) {
    m_success = true; m_inFlight = false;
    return m_db->finalizeTopLevelSync(generation, m_metadata);
}
std::future<CatalogDbTopLevelSyncResult> library::LibrarySync::abort(std::uint64_t generation) {
    m_inFlight = false;
    return m_db->abortTopLevelSync(generation, m_metadata);
}
std::future<CatalogDbReconcileResult>
library::LibrarySync::reconcileSeries(
    const std::vector<MediaItem> &series, bool authoritative,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    CatalogDbJobMetadata metadata = m_metadata;
    metadata.cancellation = cancellation ? cancellation : m_cancel;
    return m_db->reconcileSeries(series, authoritative, metadata);
}

std::future<CatalogDbHierarchyWriteResult>
library::LibrarySync::stageSeriesHierarchy(
    const MediaItem &series, const std::vector<MediaItem> &seasons,
    const std::map<std::string, std::vector<MediaItem>> &episodesBySeason,
    std::uint64_t generation, bool complete,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    if (!complete)
        return rejectedHierarchyWrite(
            "incomplete hierarchy is not eligible for CatalogDb commit");
    if (!m_db)
        return rejectedHierarchyWrite("CatalogDb service is unavailable");
    CatalogDbJobMetadata metadata = m_metadata;
    metadata.cancellation = cancellation ? cancellation : m_cancel;
    return m_db->stageSeriesHierarchy(series, seasons, episodesBySeason,
                                       generation,
                                       static_cast<std::int64_t>(std::time(nullptr)) * 1000,
                                       true, metadata);
}

std::future<CatalogDbSyncState> library::LibrarySync::writeSyncState(
    std::int64_t lastSuccessfulMs, std::int64_t lastReconcileMs,
    std::uint64_t committedGeneration,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    CatalogDbJobMetadata metadata = m_metadata;
    metadata.cancellation = cancellation ? cancellation : m_cancel;
    return m_db->writeSyncState(lastSuccessfulMs, lastReconcileMs,
                                committedGeneration, metadata);
}

std::future<library::OfflineRebuildResult>
library::LibrarySync::reconstructOfflineDownloads(
    const std::string &downloadRoot)
{
    const auto db = m_db;
    const Session session = m_session;
    const CatalogDbJobMetadata metadata = m_metadata;
    const auto cancellation = m_cancel;
    const auto reconstructionGeneration = m_offlineGeneration;
    const auto operationCancellation =
        std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard<std::mutex> lock(m_offlineMutex);
        if (m_offlineCancellation)
            m_offlineCancellation->store(true);
        m_offlineCancellation = operationCancellation;
    }
    const std::uint64_t generation = ++(*reconstructionGeneration);
    return std::async(std::launch::async,
        [db, session, metadata, cancellation, reconstructionGeneration,
         operationCancellation, generation, downloadRoot] {
            OfflineRebuildResult result;
            const auto current = [&] {
                if (cancellation && cancellation->load()) {
                    result.cancelled = true;
                    result.error = CatalogDbErrorCategory::Superseded;
                    result.message = "offline catalog reconstruction cancelled";
                    return false;
                }
                if (operationCancellation->load()) {
                    result.superseded = true;
                    result.error = CatalogDbErrorCategory::Superseded;
                    result.message =
                        "offline catalog reconstruction superseded";
                    return false;
                }
                if (reconstructionGeneration->load() != generation) {
                    result.superseded = true;
                    result.error = CatalogDbErrorCategory::Superseded;
                    result.message = "offline catalog reconstruction superseded";
                    return false;
                }
                return true;
            };
            if (!current()) return result;

            DownloadStore store(downloadRoot);
            const std::string scope =
                DownloadStore::scopeKey(session.serverUrl, session.userId);
            std::vector<DownloadItem> downloads;
            std::string error;
            if (!store.loadCompleteMetadata(scope, downloads, &error)) {
                const std::string indexPath = store.scopePath(scope) + "/index.v1";
                const bool indexMissing = ::access(indexPath.c_str(), F_OK) != 0;
                const int accessError = errno;
                if (indexMissing && accessError == ENOENT) {
                    result.success = true;
                    result.skipped = true;
                    result.message = "no durable download metadata";
                    return result;
                }
                result.error = CatalogDbErrorCategory::CorruptOrIo;
                result.message = error.empty()
                    ? "download metadata could not be read" : error;
                return result;
            }

            const OfflineLibraryQuery::Hierarchy hierarchy =
                OfflineLibraryQuery::hierarchy(downloads);
            CatalogDbJobMetadata writeMetadata = metadata;
            writeMetadata.cancellation = operationCancellation;
            const auto write = [&](const MediaItem &root,
                                   const std::vector<MediaItem> &seasons,
                                   const std::map<std::string,
                                                  std::vector<MediaItem>> &episodes) {
                if (!current()) return false;
                const auto written = db->stageSeriesHierarchy(
                    root, seasons, episodes, 0, 0, false, writeMetadata).get();
                result.itemsUpserted += written.rowsWritten;
                if (written.cancelled) {
                    result.cancelled = true;
                    result.error = written.error;
                    result.message = written.message;
                    return false;
                }
                if (written.superseded) {
                    result.superseded = true;
                    result.error = written.error;
                    result.message = written.message;
                    return false;
                }
                if (!written.success) {
                    result.error = written.error;
                    result.message = written.message;
                    return false;
                }
                return true;
            };

            for (const auto &movie : hierarchy.movies) {
                if (!write(movie, {}, {})) return result;
            }
            result.containersSynthesized =
                hierarchy.series.size() + hierarchy.seasons.size();
            for (const auto &seriesEntry : hierarchy.series) {
                std::vector<MediaItem> seasons;
                std::map<std::string, std::vector<MediaItem>> episodes;
                for (const auto &seasonEntry : hierarchy.seasons) {
                    if (seasonEntry.second.seriesId != seriesEntry.first)
                        continue;
                    seasons.push_back(seasonEntry.second);
                    const auto found = hierarchy.episodesBySeason.find(
                        seasonEntry.first);
                    episodes.emplace(seasonEntry.first,
                                     found == hierarchy.episodesBySeason.end()
                                         ? std::vector<MediaItem>{}
                                         : found->second);
                }
                if (!write(seriesEntry.second, seasons, episodes))
                    return result;
            }
            if (!current()) return result;
            result.success = true;
            return result;
        });
}
void library::LibrarySync::cancel() noexcept
{
    if (m_cancel)
        m_cancel->store(true);
    {
        std::lock_guard<std::mutex> lock(m_liveEventMutex);
        if (m_liveEventCancellation)
            m_liveEventCancellation->store(true);
    }
    std::lock_guard<std::mutex> lock(m_offlineMutex);
    if (m_offlineCancellation)
        m_offlineCancellation->store(true);
}

void library::LibrarySync::stop() noexcept
{
    cancel();
    if (m_liveEventThread.joinable())
        m_liveEventThread.join();
}
}
