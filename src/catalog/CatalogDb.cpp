#include "CatalogDb.hpp"

#include "../cache/LibraryCache.hpp"
#include "../net/JellyfinApi.hpp"

namespace miyoofin {

namespace {

bool validScopeIdentity(const std::string &serverUrl, const std::string &userId)
{
    const std::string normalizedUrl = JellyfinApi::normaliseUrl(serverUrl);
    const std::size_t schemeEnd = normalizedUrl.find("://");
    return schemeEnd != std::string::npos
        && schemeEnd + 3 < normalizedUrl.size()
        && userId.find_first_not_of(" \t\r\n") != std::string::npos;
}

} // namespace

CatalogDb::CatalogDb()
    : m_worker(&CatalogDb::workerLoop, this)
{
}

CatalogDb::~CatalogDb()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
        for (auto &queue : m_queues) {
            queue.clear();
        }
        m_scopeCommands.clear();
        m_pendingJobs = 0;
    }
    m_wake.notify_one();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

CatalogDbEnqueueResult CatalogDb::enqueueNoopForTest(
    CatalogDbPriority priority,
    const CatalogDbJobMetadata &metadata)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            return CatalogDbEnqueueResult::RejectedStopping;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            return CatalogDbEnqueueResult::RejectedCancelled;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            return CatalogDbEnqueueResult::RejectedFull;
        }
        m_queues[priorityIndex(priority)].push_back({priority, metadata});
        ++m_pendingJobs;
    }
    m_wake.notify_one();
    return CatalogDbEnqueueResult::Accepted;
}

std::uint64_t CatalogDb::configureScope(const std::string &serverUrl,
                                        const std::string &userId)
{
    const bool validIdentity = validScopeIdentity(serverUrl, userId);
    const std::string scopeKey = validIdentity
        ? LibraryCache::scopeKey(serverUrl, userId) : std::string();
    std::uint64_t epoch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        epoch = ++m_requestedEpoch;
        m_requestedScopeKey = scopeKey;
        m_activeScopeKey.clear();
        m_scopeConfigured = false;
        m_scopeReady = false;
        m_scopeStatus = validIdentity ? CatalogDbScopeStatus::Pending
                                      : CatalogDbScopeStatus::InvalidIdentity;
        m_scopeCommands.clear();
        m_scopeCommands.push_back({
            validIdentity ? ScopeCommandKind::Configure
                          : ScopeCommandKind::InvalidIdentity,
            epoch, scopeKey});
    }
    m_wake.notify_one();
    return epoch;
}

std::uint64_t CatalogDb::deconfigureScope()
{
    std::uint64_t epoch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        epoch = ++m_requestedEpoch;
        m_requestedScopeKey.clear();
        m_activeScopeKey.clear();
        m_scopeConfigured = false;
        m_scopeReady = false;
        m_scopeStatus = CatalogDbScopeStatus::Unconfigured;
        m_scopeCommands.clear();
        m_scopeCommands.push_back({ScopeCommandKind::Deconfigure, epoch, {}});
    }
    m_wake.notify_one();
    return epoch;
}

CatalogDbScopeState CatalogDb::scopeState() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_requestedEpoch, m_scopeConfigured, m_scopeReady, m_scopeStatus};
}

CatalogDbEnqueueResult CatalogDb::enqueueScopedNoopForTest(
    CatalogDbPriority priority)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping) {
        return CatalogDbEnqueueResult::RejectedStopping;
    }
    if (!m_scopeConfigured || !m_scopeReady) {
        return CatalogDbEnqueueResult::RejectedScopeNotReady;
    }
    if (m_pendingJobs >= kMaxPendingJobs) {
        return CatalogDbEnqueueResult::RejectedFull;
    }
    CatalogDbJobMetadata metadata;
    metadata.generation = m_generation;
    metadata.scopeEpoch = m_requestedEpoch;
    m_queues[priorityIndex(priority)].push_back({priority, metadata});
    ++m_pendingJobs;
    m_wake.notify_one();
    return CatalogDbEnqueueResult::Accepted;
}

bool CatalogDb::canPublishForTest(std::uint64_t scopeEpoch) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return scopeEpoch != 0 && scopeEpoch == m_requestedEpoch
        && m_scopeConfigured && m_scopeReady;
}

void CatalogDb::setGenerationForTest(std::uint64_t generation)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_generation = generation;
    }
    m_wake.notify_one();
}

void CatalogDb::setWorkerPausedForTest(bool paused)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pausedForTest = paused;
    }
    m_wake.notify_one();
}

bool CatalogDb::waitForIdleForTest(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_idle.wait_for(lock, timeout, [this] {
        return m_pendingJobs == 0 && m_scopeCommands.empty() && !m_runningJob;
    });
}

std::vector<CatalogDbJobReport> CatalogDb::jobReportsForTest() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_jobReports.begin(), m_jobReports.end()};
}

void CatalogDb::workerLoop()
{
    for (;;) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_wake.wait(lock, [this] {
            return m_stopping || (!m_pausedForTest
                && (!m_scopeCommands.empty() || hasPendingJobsLocked()));
        });

        if (m_stopping) {
            return;
        }

        if (!m_scopeCommands.empty()) {
            ScopeCommand command = std::move(m_scopeCommands.front());
            m_scopeCommands.pop_front();
            lock.unlock();
            processScopeCommand(std::move(command));
            continue;
        }

        Job job = takeNextJobLocked();
        m_runningJob = true;
        lock.unlock();

        CatalogDbJobDisposition disposition = CatalogDbJobDisposition::Completed;
        if (job.metadata.cancellation && job.metadata.cancellation->load()) {
            disposition = CatalogDbJobDisposition::Cancelled;
        } else {
            std::lock_guard<std::mutex> stateLock(m_mutex);
            if (job.metadata.generation != m_generation
                || (job.metadata.scopeEpoch != 0
                    && (job.metadata.scopeEpoch != m_requestedEpoch
                        || !m_scopeConfigured || !m_scopeReady))) {
                disposition = CatalogDbJobDisposition::Superseded;
            }
        }

        lock.lock();
        if (m_jobReports.size() == kMaxPendingJobs) {
            m_jobReports.pop_front();
        }
        m_jobReports.push_back({job.priority, job.metadata, disposition});
        m_runningJob = false;
        if (m_pendingJobs == 0) {
            m_idle.notify_all();
        }
    }
}

bool CatalogDb::hasPendingJobsLocked() const
{
    return m_pendingJobs != 0;
}

CatalogDb::Job CatalogDb::takeNextJobLocked()
{
    for (std::size_t index = 0; index < m_queues.size(); ++index) {
        if (!m_queues[index].empty()) {
            Job job = m_queues[index].front();
            m_queues[index].pop_front();
            --m_pendingJobs;
            return job;
        }
    }

    return {CatalogDbPriority::Maintenance, {}};
}

std::size_t CatalogDb::priorityIndex(CatalogDbPriority priority)
{
    return static_cast<std::size_t>(priority);
}

void CatalogDb::processScopeCommand(ScopeCommand command)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch != m_requestedEpoch) {
            m_idle.notify_all();
            return;
        }

        switch (command.kind) {
        case ScopeCommandKind::Configure:
            m_activeScopeKey = std::move(command.scopeKey);
            m_scopeConfigured = true;
            m_scopeReady = true;
            m_scopeStatus = CatalogDbScopeStatus::Ready;
            break;
        case ScopeCommandKind::InvalidIdentity:
            m_activeScopeKey.clear();
            m_scopeConfigured = false;
            m_scopeReady = false;
            m_scopeStatus = CatalogDbScopeStatus::InvalidIdentity;
            break;
        case ScopeCommandKind::Deconfigure:
            m_activeScopeKey.clear();
            m_scopeConfigured = false;
            m_scopeReady = false;
            m_scopeStatus = CatalogDbScopeStatus::Unconfigured;
            break;
        }
    }
    m_idle.notify_all();
}

} // namespace miyoofin
