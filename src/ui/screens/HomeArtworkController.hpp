#ifndef MIYOOFIN_HOME_ARTWORK_CONTROLLER_HPP
#define MIYOOFIN_HOME_ARTWORK_CONTROLLER_HPP

#include "../../image/ImageDecoder.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/Session.hpp"
#include "../../ui/HomeArtworkPlan.hpp"
#include "../../diagnostics/TelemetryIds.hpp"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace miyoofin {

/// Owns Home's blocking artwork work.  The controller has no presentation or
/// SDL knowledge; workers publish value results for Home to apply on its UI
/// thread.
class HomeArtworkController
{
  public:
    static constexpr int kPosterThreads = 3;
    static constexpr int kDecodeQueueLimit = 32;
    static constexpr int kMaxDecodeAttempts = 3;

    struct RequestIdentity
    {
        std::string itemId;
        ImageType imageType = ImageType::Primary;
        std::string imageTag;
        int width = 0;
        int height = 0;

        bool operator==(const RequestIdentity& other) const
        {
            return itemId == other.itemId && imageType == other.imageType &&
                   imageTag == other.imageTag && width == other.width && height == other.height;
        }
        bool operator<(const RequestIdentity& other) const
        {
            if (itemId != other.itemId)
                return itemId < other.itemId;
            if (imageType != other.imageType)
                return static_cast<int>(imageType) < static_cast<int>(other.imageType);
            if (imageTag != other.imageTag)
                return imageTag < other.imageTag;
            if (width != other.width)
                return width < other.width;
            return height < other.height;
        }
    };

    struct DecodeRequest
    {
        RequestIdentity identity;
        bool highPriority = false;
        ArtworkContext context = ArtworkContext::HomeGrid;
        std::uint64_t showsWorkingSetGeneration = 0;
    };

    /// Immutable worker output.  Home decides whether the result is still
    /// fresh enough to affect presentation state; only accepted results are
    /// finalized into the controller's three-attempt tombstone.
    struct DecodeResult
    {
        RequestIdentity identity;
        DecodedImage image;
        std::uint64_t requestSequence = 0;
        std::uint64_t showsWorkingSetGeneration = 0;
        ArtworkContext context = ArtworkContext::HomeGrid;
        bool cachePresent = false;
        bool terminalFailure = false;
    };

    struct ArtworkProgress
    {
        std::size_t completed = 0;
        std::size_t total = 0;
        bool active = false;
    };

    /// `startWorkers=false` is a deterministic test seam for queue-bound and
    /// stale-admission assertions; production always uses the default.
    explicit HomeArtworkController(const Session& session, int posterThreads = kPosterThreads,
                                   bool startWorkers = true);
    ~HomeArtworkController();

    HomeArtworkController(const HomeArtworkController&) = delete;
    HomeArtworkController& operator=(const HomeArtworkController&) = delete;

    static std::string identityKey(const RequestIdentity& identity);
    static std::string posterJobKey(const HomePosterJob& job);
    static bool shouldProcessPosterJob(bool highPriority, bool lowPriorityDeferred)
    {
        return highPriority || !lowPriorityDeferred;
    }

    void queuePosterJobs(std::vector<HomePosterJob> jobs, bool highPriority = false);
    void setLowPriorityDeferred(bool deferred);

    void requestDecode(DecodeRequest request);
    /// Reconcile queued Shows work with Home's current working set.  In-flight
    /// work is intentionally not cancelled; its generation is returned so
    /// Home can reject stale immutable results.
    void setShowsWorkingSet(std::uint64_t generation,
                            const std::set<std::string>& desiredIdentityKeys);
    void takeDecodeResults(std::deque<DecodeResult>& results);
    /// Complete one UI-consumed result.  Rejected stale results release
    /// admission without consuming retry budget; accepted results update the
    /// cache-miss/corrupt-JPEG attempt state and terminal tombstone.
    void completeDecodeResult(DecodeResult& result, bool accepted);

    ArtworkProgress artworkProgress() const;
    std::size_t queuedPosterJobs() const;
    std::vector<std::string> queuedPosterJobKeys(bool highPriority) const;
    std::size_t queuedDecodeJobs() const;
    bool admissionClosed() const;
    bool workersJoined() const;

    /// Close admission, wake every worker, and make teardown safe.  Joining
    /// remains a separate operation so Home can stop all dependent workers
    /// before joining them in its existing lifecycle order.
    void requestStopAllWorkers() noexcept;
    void joinAllWorkers();

  private:
    struct DecodeJob
    {
        DecodeRequest request;
        std::uint64_t sequence = 0;
    };

    void posterWorker();
    void decodeWorker();
    void publishPosterCompletion(const HomePosterJob& job);
    void publishDecodeResult(DecodeJob job, std::vector<unsigned char> bytes, DecodedImage image);

    Session m_session;

    std::vector<std::thread> m_posterThreads;
    mutable std::mutex m_posterMutex;
    std::condition_variable m_posterWake;
    std::deque<HomePosterJob> m_highPriorityPosterJobs;
    std::deque<HomePosterJob> m_lowPriorityPosterJobs;
    std::set<std::string> m_artworkProgressKeys;
    bool m_lowPriorityDeferred = false;
    bool m_stopPosterWorker = false;

    std::atomic<std::size_t> m_artworkCompleted{0};
    std::atomic<std::size_t> m_artworkTotal{0};
    std::atomic<bool> m_artworkActive{false};

    mutable std::mutex m_decodeMutex;
    std::condition_variable m_decodeWake;
    std::deque<DecodeJob> m_highPriorityDecodeJobs;
    std::deque<DecodeJob> m_lowPriorityDecodeJobs;
    std::deque<DecodeResult> m_decodeResults;
    std::set<std::string> m_decodeOutstanding;
    std::map<std::string, int> m_decodeAttempts;
    std::uint64_t m_nextDecodeSequence = 1;
    std::thread m_decodeThread;
    bool m_stopDecodeWorker = false;

    std::atomic<bool> m_admissionClosed{false};
    std::atomic<bool> m_workersJoined{false};
};

} // namespace miyoofin

#endif // MIYOOFIN_HOME_ARTWORK_CONTROLLER_HPP
