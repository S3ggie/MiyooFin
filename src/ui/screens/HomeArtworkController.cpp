#include "HomeArtworkController.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../diagnostics/PerformanceTelemetry.hpp"
#include "../../diagnostics/TelemetryGuards.hpp"
#include "../../net/HttpClient.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/RouteRequest.hpp"
#include <algorithm>

namespace miyoofin {

HomeArtworkController::HomeArtworkController(const Session& session, int posterThreads,
                                             bool startWorkers)
    : m_session(session)
{
    if (!startWorkers)
        return;
    const int count = std::max(1, posterThreads);
    m_posterThreads.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        m_posterThreads.emplace_back(&HomeArtworkController::posterWorker, this);
    m_decodeThread = std::thread(&HomeArtworkController::decodeWorker, this);
}

HomeArtworkController::~HomeArtworkController()
{
    requestStopAllWorkers();
    joinAllWorkers();
}

std::string HomeArtworkController::identityKey(const RequestIdentity& identity)
{
    return identity.itemId + ":" + imageTypeName(identity.imageType) + ":" + identity.imageTag +
           ":" + std::to_string(identity.width) + "x" + std::to_string(identity.height);
}

std::string HomeArtworkController::posterJobKey(const HomePosterJob& job)
{
    return job.itemId + ":" + std::to_string(static_cast<int>(job.imageType)) + ":" + job.imageTag +
           ":" + std::to_string(job.width) + "x" + std::to_string(job.height);
}

void HomeArtworkController::setLowPriorityDeferred(bool deferred)
{
    {
        std::lock_guard<std::mutex> lock(m_posterMutex);
        if (m_lowPriorityDeferred == deferred)
            return;
        m_lowPriorityDeferred = deferred;
    }
    m_posterWake.notify_all();
}

void HomeArtworkController::queuePosterJobs(std::vector<HomePosterJob> jobs, bool highPriority)
{
    std::lock_guard<std::mutex> lock(m_posterMutex);
    if (m_admissionClosed.load() || m_stopPosterWorker)
        return;

    std::vector<HomePosterJob> highPriorityJobs;
    std::vector<HomePosterJob> newJobs;
    highPriorityJobs.reserve(jobs.size());
    newJobs.reserve(jobs.size());
    for (auto& job : jobs) {
        const std::string key = posterJobKey(job);
        if (highPriority) {
            auto queued = std::find_if(
                m_lowPriorityPosterJobs.begin(), m_lowPriorityPosterJobs.end(),
                [&](const HomePosterJob& candidate) { return posterJobKey(candidate) == key; });
            if (queued != m_lowPriorityPosterJobs.end()) {
                highPriorityJobs.push_back(std::move(*queued));
                m_lowPriorityPosterJobs.erase(queued);
                continue;
            }
        }
        if (m_artworkProgressKeys.insert(key).second) {
            if (highPriority)
                highPriorityJobs.push_back(std::move(job));
            else
                newJobs.push_back(std::move(job));
            m_artworkTotal.fetch_add(1);
            m_artworkActive.store(true);
        }
    }

    auto& target = highPriority ? m_highPriorityPosterJobs : m_lowPriorityPosterJobs;
    if (highPriority) {
        // Insert the complete caller batch in reverse so its original order
        // is retained, including the relative positions of promotions and
        // newly admitted jobs.
        for (auto it = highPriorityJobs.rbegin(); it != highPriorityJobs.rend(); ++it)
            target.push_front(std::move(*it));
    } else {
        for (auto& job : newJobs)
            target.push_back(std::move(job));
    }
    performanceTelemetry().setWorkerQueueDepth(
        WorkerId::HomePoster,
        static_cast<uint32_t>(m_highPriorityPosterJobs.size() + m_lowPriorityPosterJobs.size()));
    m_posterWake.notify_all();
}

void HomeArtworkController::requestDecode(DecodeRequest request)
{
    const std::string key = identityKey(request.identity);
    if (key.empty() || request.identity.width <= 0 || request.identity.height <= 0)
        return;

    std::lock_guard<std::mutex> lock(m_decodeMutex);
    if (m_admissionClosed.load() || m_stopDecodeWorker || !m_decodeOutstanding.insert(key).second)
        return;

    const std::size_t queued = m_highPriorityDecodeJobs.size() + m_lowPriorityDecodeJobs.size();
    if (queued >= kDecodeQueueLimit) {
        m_decodeOutstanding.erase(key);
        performanceTelemetry().addWorkerFailed(WorkerId::HomeDecode);
        return;
    }

    DecodeJob job{std::move(request), m_nextDecodeSequence++};
    if (job.request.highPriority)
        m_highPriorityDecodeJobs.push_front(std::move(job));
    else
        m_lowPriorityDecodeJobs.push_back(std::move(job));
    performanceTelemetry().setWorkerQueueDepth(
        WorkerId::HomeDecode,
        static_cast<uint32_t>(m_highPriorityDecodeJobs.size() + m_lowPriorityDecodeJobs.size()));
    m_decodeWake.notify_one();
}

void HomeArtworkController::setShowsWorkingSet(std::uint64_t generation,
                                               const std::set<std::string>& desiredIdentityKeys)
{
    std::lock_guard<std::mutex> lock(m_decodeMutex);
    const auto removeStale = [&](std::deque<DecodeJob>& jobs) {
        for (auto it = jobs.begin(); it != jobs.end();) {
            const bool stale = it->request.context == ArtworkContext::HomeShows &&
                               (it->request.showsWorkingSetGeneration != generation ||
                                desiredIdentityKeys.find(identityKey(it->request.identity)) ==
                                    desiredIdentityKeys.end());
            if (stale) {
                m_decodeOutstanding.erase(identityKey(it->request.identity));
                it = jobs.erase(it);
            } else {
                ++it;
            }
        }
    };
    removeStale(m_highPriorityDecodeJobs);
    removeStale(m_lowPriorityDecodeJobs);
    performanceTelemetry().setWorkerQueueDepth(
        WorkerId::HomeDecode,
        static_cast<uint32_t>(m_highPriorityDecodeJobs.size() + m_lowPriorityDecodeJobs.size()));
}

void HomeArtworkController::takeDecodeResults(std::deque<DecodeResult>& results)
{
    std::lock_guard<std::mutex> lock(m_decodeMutex);
    results.swap(m_decodeResults);
}

void HomeArtworkController::completeDecodeResult(DecodeResult& result, bool accepted)
{
    std::lock_guard<std::mutex> lock(m_decodeMutex);
    const std::string key = identityKey(result.identity);
    if (!accepted) {
        m_decodeOutstanding.erase(key);
        return;
    }

    if (result.cachePresent && result.image.empty()) {
        ImageCache::removeCached(result.identity.itemId, result.identity.imageType,
                                 result.identity.imageTag, result.identity.width,
                                 result.identity.height);
        const int attempts = ++m_decodeAttempts[key];
        result.terminalFailure = attempts >= kMaxDecodeAttempts;
    } else if (!result.cachePresent) {
        // A miss is retryable on the existing Home cadence, but it still
        // counts toward the bounded decode/tombstone contract.  Unlike a
        // corrupt JPEG, there is no file to remove and a later poster sync
        // may replace the miss with a valid cache entry.
        const int attempts = ++m_decodeAttempts[key];
        result.terminalFailure = attempts >= kMaxDecodeAttempts;
    } else if (!result.image.empty()) {
        m_decodeAttempts.erase(key);
    }
    m_decodeOutstanding.erase(key);
}

HomeArtworkController::ArtworkProgress HomeArtworkController::artworkProgress() const
{
    return {m_artworkCompleted.load(), m_artworkTotal.load(), m_artworkActive.load()};
}

std::size_t HomeArtworkController::queuedPosterJobs() const
{
    std::lock_guard<std::mutex> lock(m_posterMutex);
    return m_highPriorityPosterJobs.size() + m_lowPriorityPosterJobs.size();
}

std::vector<std::string> HomeArtworkController::queuedPosterJobKeys(bool highPriority) const
{
    std::lock_guard<std::mutex> lock(m_posterMutex);
    const auto& jobs = highPriority ? m_highPriorityPosterJobs : m_lowPriorityPosterJobs;
    std::vector<std::string> keys;
    keys.reserve(jobs.size());
    for (const auto& job : jobs)
        keys.push_back(posterJobKey(job));
    return keys;
}

std::size_t HomeArtworkController::queuedDecodeJobs() const
{
    std::lock_guard<std::mutex> lock(m_decodeMutex);
    return m_highPriorityDecodeJobs.size() + m_lowPriorityDecodeJobs.size();
}

bool HomeArtworkController::admissionClosed() const
{
    return m_admissionClosed.load();
}

bool HomeArtworkController::workersJoined() const
{
    return m_workersJoined.load();
}

void HomeArtworkController::requestStopAllWorkers() noexcept
{
    // Take both admission locks before publishing closure.  Producers check
    // the same atomic while holding their queue lock, so none can pass its
    // admission check after logical closure or race an already-admitted job.
    std::scoped_lock locks(m_posterMutex, m_decodeMutex);
    m_admissionClosed.store(true);
    m_stopPosterWorker = true;
    m_stopDecodeWorker = true;
    m_posterWake.notify_all();
    m_decodeWake.notify_all();
}

void HomeArtworkController::joinAllWorkers()
{
    for (auto& thread : m_posterThreads) {
        if (thread.joinable())
            thread.join();
    }
    if (m_decodeThread.joinable())
        m_decodeThread.join();
    m_workersJoined.store(true);
}

void HomeArtworkController::publishPosterCompletion(const HomePosterJob& job)
{
    m_artworkCompleted.fetch_add(1);
    std::lock_guard<std::mutex> lock(m_posterMutex);
    m_artworkProgressKeys.erase(posterJobKey(job));
    if (m_artworkCompleted.load() >= m_artworkTotal.load() && m_highPriorityPosterJobs.empty() &&
        m_lowPriorityPosterJobs.empty())
        m_artworkActive.store(false);
    performanceTelemetry().setWorkerQueueDepth(
        WorkerId::HomePoster,
        static_cast<uint32_t>(m_highPriorityPosterJobs.size() + m_lowPriorityPosterJobs.size()));
}

void HomeArtworkController::posterWorker()
{
    HttpClient client;
    client.setTimeoutSec(8);
    for (;;) {
        HomePosterJob job;
        {
            std::unique_lock<std::mutex> lock(m_posterMutex);
            m_posterWake.wait(lock, [&] {
                return m_stopPosterWorker || !m_highPriorityPosterJobs.empty() ||
                       (!m_lowPriorityDeferred && !m_lowPriorityPosterJobs.empty());
            });
            if (m_stopPosterWorker)
                return;
            if (!m_highPriorityPosterJobs.empty()) {
                job = std::move(m_highPriorityPosterJobs.front());
                m_highPriorityPosterJobs.pop_front();
            } else if (!m_lowPriorityDeferred && !m_lowPriorityPosterJobs.empty()) {
                job = std::move(m_lowPriorityPosterJobs.front());
                m_lowPriorityPosterJobs.pop_front();
            } else {
                continue;
            }
            performanceTelemetry().setWorkerQueueDepth(
                WorkerId::HomePoster, static_cast<uint32_t>(m_highPriorityPosterJobs.size() +
                                                            m_lowPriorityPosterJobs.size()));
            performanceTelemetry().setWorkerActive(WorkerId::HomePoster, true);
        }

        bool complete =
            ImageCache::isCached(job.itemId, job.imageType, job.imageTag, job.width, job.height);
        if (!complete) {
            BinaryHttpResponse response;
            std::string error;
            TelemetryRequestScope request(RequestKind::Artwork);
            TelemetryArtworkScope artwork(ArtworkContext::HomePoster);
            if (RouteRequest(m_session).run(
                    [&](const std::string& base) {
                        return client.getBinary(buildImageUrl(base, job.itemId, job.imageType,
                                                              job.imageTag, job.width, job.height),
                                                JellyfinApi::buildAuthHeaders(m_session.accessToken,
                                                                              m_session.deviceId),
                                                response, error, 512 * 1024, &m_admissionClosed) &&
                               response.ok();
                    },
                    error) &&
                !response.data.empty()) {
                complete = ImageCache::writeToCache(job.itemId, job.imageType, job.imageTag,
                                                    job.width, job.height, response.data.data(),
                                                    response.data.size());
            }
        }
        if (complete)
            performanceTelemetry().addWorkerCompleted(WorkerId::HomePoster);
        else
            performanceTelemetry().addWorkerFailed(WorkerId::HomePoster);
        publishPosterCompletion(job);
        performanceTelemetry().setWorkerActive(WorkerId::HomePoster, false);
    }
}

void HomeArtworkController::publishDecodeResult(DecodeJob job, std::vector<unsigned char> bytes,
                                                DecodedImage image)
{
    DecodeResult result;
    result.identity = job.request.identity;
    result.image = std::move(image);
    result.requestSequence = job.sequence;
    result.showsWorkingSetGeneration = job.request.showsWorkingSetGeneration;
    result.context = job.request.context;
    result.cachePresent = !bytes.empty();

    std::lock_guard<std::mutex> lock(m_decodeMutex);
    if (!m_stopDecodeWorker)
        m_decodeResults.push_back(std::move(result));
}

void HomeArtworkController::decodeWorker()
{
    for (;;) {
        DecodeJob job;
        {
            std::unique_lock<std::mutex> lock(m_decodeMutex);
            m_decodeWake.wait(lock, [&] {
                return m_stopDecodeWorker || !m_highPriorityDecodeJobs.empty() ||
                       !m_lowPriorityDecodeJobs.empty();
            });
            if (m_stopDecodeWorker)
                return;
            if (!m_highPriorityDecodeJobs.empty()) {
                job = std::move(m_highPriorityDecodeJobs.front());
                m_highPriorityDecodeJobs.pop_front();
            } else {
                job = std::move(m_lowPriorityDecodeJobs.front());
                m_lowPriorityDecodeJobs.pop_front();
            }
            performanceTelemetry().setWorkerQueueDepth(
                WorkerId::HomeDecode, static_cast<uint32_t>(m_highPriorityDecodeJobs.size() +
                                                            m_lowPriorityDecodeJobs.size()));
            performanceTelemetry().setWorkerActive(WorkerId::HomeDecode, true);
        }

        TelemetryArtworkScope artwork(job.request.context);
        auto bytes = ImageCache::readCached(
            job.request.identity.itemId, job.request.identity.imageType,
            job.request.identity.imageTag, job.request.identity.width, job.request.identity.height);
        DecodedImage image =
            bytes.empty() ? DecodedImage{} : ImageDecoder::decodeJpeg(bytes.data(), bytes.size());
        publishDecodeResult(std::move(job), std::move(bytes), std::move(image));
        performanceTelemetry().setWorkerActive(WorkerId::HomeDecode, false);
    }
}

} // namespace miyoofin
