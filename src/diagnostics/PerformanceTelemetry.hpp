#ifndef MIYOOFIN_PERFORMANCE_TELEMETRY_HPP
#define MIYOOFIN_PERFORMANCE_TELEMETRY_HPP

#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1
#include <array>
#include <atomic>
#include <thread>
#endif
#include <cstdint>

#include "TelemetryConfig.hpp"
#include "TelemetryIds.hpp"
#include "TelemetryRing.hpp"
#include "TelemetryTypes.hpp"
#include "TelemetryWriter.hpp"
#include "LinuxProcessMetrics.hpp"
#include "../input/Action.hpp"

namespace miyoofin {

#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1

class PerformanceTelemetry
{
public:
#if defined(MIYOOFIN_TELEMETRY_HOST_TEST)
    using MonotonicUsHook = uint64_t (*)() noexcept;
    using ProcessMetricsHook = LinuxProcessMetricsSnapshot (*)(const LinuxProcessMetrics &, bool) noexcept;
    struct TestHooks {
        MonotonicUsHook monotonicUs = nullptr;
        ProcessMetricsHook processMetrics = nullptr;
    };

    static void setTestHooks(const TestHooks &hooks) noexcept;
    static void clearTestHooks() noexcept;
    static void setSchemaVersionForTest(uint16_t version) noexcept;
#endif

    void start(const TelemetryConfig &config);
    void stop() noexcept;
    bool enabledFast() const noexcept;
    void suspendSampling(bool suspended, SamplingReason reason = SamplingReason::Unknown) noexcept;
    uint64_t nextEphemeralId() noexcept;
    bool emitRecord(const TelemetryRecord &record) noexcept;
    bool emitSessionEvent(SessionEventKind kind,
                          Outcome outcome,
                          uint32_t value0,
                          uint64_t value1) noexcept;
    uint64_t droppedRecordCount() const noexcept;
    uint64_t writerErrorCount() const noexcept;
    uint32_t rotationCount() const noexcept;
    uint32_t samplingLateCount() const noexcept;
    bool serviceThreadActive() const noexcept;

    static ScreenId screenIdFromDiagnosticName(const char *name) noexcept;
    static TabId tabIdFromDiagnosticName(const char *name) noexcept;
    static ActionId actionIdFromAction(Action action) noexcept;
    void setScreen(ScreenId screen) noexcept;
    void setTab(TabId tab) noexcept;
    void setAction(ActionId action) noexcept;
    void setPlaybackState(PlaybackState state) noexcept;

    void setWorkerActive(WorkerId worker, bool active) noexcept;
    void setWorkerQueueDepth(WorkerId worker, uint32_t depth) noexcept;
    void addWorkerCompleted(WorkerId worker, uint32_t count = 1) noexcept;
    void addWorkerFailed(WorkerId worker, uint32_t count = 1) noexcept;
    void addWorkerCancelled(WorkerId worker, uint32_t count = 1) noexcept;

    void recordArtworkCacheProbe(bool hit) noexcept;
    void recordArtworkCacheRead(bool success, uint64_t compressedBytes) noexcept;
    void recordArtworkCacheWrite(bool success, uint64_t compressedBytes) noexcept;
    void recordArtworkDecode(bool success, uint64_t durationUs) noexcept;
    void recordFramePhase(FramePhase phase, uint64_t durationUs) noexcept;

    void setDownloadGauges(uint32_t activeDownloads,
                           uint32_t queuedDownloads,
                           uint32_t plannerQueueDepth) noexcept;
    void addDownloadBytes(uint64_t bytes) noexcept;
    void addDownloadSegmentCompleted(uint32_t count = 1) noexcept;
    void addDownloadSegmentRetries(uint32_t count = 1) noexcept;

    void setCatalogDbActive(bool active) noexcept;
    void setCatalogDbQueueDepth(uint32_t depth) noexcept;
    void addCatalogDbCompleted(uint32_t count = 1) noexcept;
    void addCatalogDbFailed(uint32_t count = 1) noexcept;
    void addCatalogDbCancelled(uint32_t count = 1) noexcept;
    void recordCatalogDbQuery(uint64_t durationUs) noexcept;
    void recordCatalogDbTransaction(uint64_t durationUs) noexcept;
    void recordCatalogDbCommit(uint64_t durationUs) noexcept;
    void recordCatalogDbQueueWait(uint64_t durationUs) noexcept;
    void addCatalogDbEnqueueRejected(uint32_t count = 1) noexcept;
    void addCatalogDbRows(uint32_t inserted, uint32_t updated,
                          uint32_t deleted) noexcept;
    void recordCatalogDbSqliteError(int resultCode) noexcept;

private:
    struct WorkerSlot {
        std::atomic<uint32_t> active{0};
        std::atomic<uint32_t> queueDepth{0};
        std::atomic<uint32_t> queueHighwater{0};
        std::atomic<uint32_t> completed{0};
        std::atomic<uint32_t> failed{0};
        std::atomic<uint32_t> cancelled{0};
    };

    struct FramePhaseAccumulator {
        std::atomic<uint32_t> count{0};
        std::atomic<uint64_t> totalUs{0};
        std::atomic<uint32_t> maxUs{0};
        std::atomic<uint32_t> over50Ms{0};
        std::atomic<uint32_t> over100Ms{0};
        std::array<std::atomic<uint32_t>, 9> histogram{};
    };

    void serviceLoop() noexcept;
    bool enqueue(TelemetryRecord record) noexcept;
    bool writeServiceRecord(TelemetryRecord record) noexcept;
    void emitWorkerSamples(uint64_t nowUs) noexcept;
    void emitArtworkSummary(uint64_t nowUs) noexcept;
    void emitDownloadSample(uint64_t nowUs, uint64_t actualIntervalUs) noexcept;
    void emitFrameTimingSummaries(uint64_t nowUs, uint64_t intervalUs) noexcept;
    void emitTelemetryHealth(uint64_t nowUs) noexcept;
    void emitCatalogDbSummary(uint64_t nowUs) noexcept;
    void emitCatalogDbWorkerSample(uint64_t nowUs) noexcept;
    void emitStateTransition(StateKind kind, uint16_t previous,
                             uint16_t current) noexcept;
    void updateQueueHighwater(uint32_t depth) noexcept;
    void updateCatalogDbQueueHighwater(uint32_t depth) noexcept;
    WorkerSlot *workerSlot(WorkerId worker) noexcept;

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_samplingSuspended{false};
    std::atomic<uint64_t> m_nextEphemeralId{1};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_serviceActive{false};
    std::atomic<uint64_t> m_droppedRecords{0};
    std::atomic<uint64_t> m_writerErrors{0};
    std::atomic<uint32_t> m_rotationCount{0};
    std::atomic<uint32_t> m_samplingLate{0};
    std::atomic<uint16_t> m_screenId{static_cast<uint16_t>(ScreenId::None)};
    std::atomic<uint16_t> m_tabId{static_cast<uint16_t>(TabId::NotApplicable)};
    std::atomic<uint16_t> m_actionId{static_cast<uint16_t>(ActionId::None)};
    std::atomic<uint16_t> m_playbackState{static_cast<uint16_t>(PlaybackState::Unknown)};
    std::atomic<uint32_t> m_transitionSequence{0};
    std::atomic<uint32_t> m_queueHighwater{0};
    std::array<WorkerSlot, 14> m_workerSlots{};
    std::array<FramePhaseAccumulator, 7> m_framePhases{};
    std::atomic<uint32_t> m_artworkCacheProbeHits{0};
    std::atomic<uint32_t> m_artworkCacheProbeMisses{0};
    std::atomic<uint32_t> m_artworkCacheReadSuccess{0};
    std::atomic<uint32_t> m_artworkCacheReadFailure{0};
    std::atomic<uint32_t> m_artworkCacheWriteSuccess{0};
    std::atomic<uint32_t> m_artworkCacheWriteFailure{0};
    std::atomic<uint64_t> m_artworkCompressedReadBytes{0};
    std::atomic<uint64_t> m_artworkCompressedWrittenBytes{0};
    std::atomic<uint32_t> m_artworkDecodeCount{0};
    std::atomic<uint32_t> m_artworkDecodeFailures{0};
    std::atomic<uint64_t> m_artworkDecodeTotalUs{0};
    std::atomic<uint32_t> m_artworkDecodeMaxUs{0};
    std::atomic<uint32_t> m_activeDownloads{0};
    std::atomic<uint32_t> m_queuedDownloads{0};
    std::atomic<uint32_t> m_plannerQueueDepth{0};
    std::atomic<uint64_t> m_downloadBytes{0};
    std::atomic<uint32_t> m_downloadSegmentsCompleted{0};
    std::atomic<uint32_t> m_downloadSegmentRetries{0};
    std::atomic<uint32_t> m_catalogDbActive{0};
    std::atomic<uint32_t> m_catalogDbQueueDepth{0};
    std::atomic<uint32_t> m_catalogDbQueueHighwater{0};
    std::atomic<uint32_t> m_catalogDbCompleted{0};
    std::atomic<uint32_t> m_catalogDbFailed{0};
    std::atomic<uint32_t> m_catalogDbCancelled{0};
    std::atomic<uint32_t> m_catalogDbQueryCount{0};
    std::atomic<uint64_t> m_catalogDbQueryTotalUs{0};
    std::atomic<uint32_t> m_catalogDbQueryMaxUs{0};
    std::atomic<uint32_t> m_catalogDbTransactionCount{0};
    std::atomic<uint64_t> m_catalogDbTransactionTotalUs{0};
    std::atomic<uint32_t> m_catalogDbTransactionMaxUs{0};
    std::atomic<uint32_t> m_catalogDbCommitCount{0};
    std::atomic<uint64_t> m_catalogDbCommitTotalUs{0};
    std::atomic<uint32_t> m_catalogDbCommitMaxUs{0};
    std::atomic<uint64_t> m_catalogDbQueueWaitTotalUs{0};
    std::atomic<uint32_t> m_catalogDbQueueWaitMaxUs{0};
    std::atomic<uint32_t> m_catalogDbEnqueueRejected{0};
    std::atomic<uint32_t> m_catalogDbRowsInserted{0};
    std::atomic<uint32_t> m_catalogDbRowsUpdated{0};
    std::atomic<uint32_t> m_catalogDbRowsDeleted{0};
    std::atomic<uint32_t> m_catalogDbBusyFamily{0};
    std::atomic<uint32_t> m_catalogDbIoerrFamily{0};
    std::atomic<uint32_t> m_catalogDbCorruptNotadb{0};
    TelemetryRing<512> m_ring;
    TelemetryWriter m_writer;
    std::thread m_serviceThread;
    uint32_t m_consumerSequence = 0;
    uint64_t m_sessionNonce = 0;
    TelemetryConfig m_config;
    uint16_t m_schemaVersion = 1;
};

PerformanceTelemetry &performanceTelemetry() noexcept;

#else

class PerformanceTelemetry
{
public:
    inline void start(const TelemetryConfig &) noexcept {}
    inline void stop() noexcept {}
    inline bool enabledFast() const noexcept { return false; }
    inline void suspendSampling(bool, SamplingReason) noexcept {}
    inline uint64_t nextEphemeralId() noexcept { return 0; }
    inline bool emitRecord(const TelemetryRecord &) noexcept { return false; }
    inline bool emitSessionEvent(SessionEventKind, Outcome, uint32_t, uint64_t) noexcept { return false; }
    inline uint64_t droppedRecordCount() const noexcept { return 0; }
    inline uint64_t writerErrorCount() const noexcept { return 0; }
    inline uint32_t rotationCount() const noexcept { return 0; }
    inline uint32_t samplingLateCount() const noexcept { return 0; }
    inline bool serviceThreadActive() const noexcept { return false; }
    static ScreenId screenIdFromDiagnosticName(const char *) noexcept { return ScreenId::Other; }
    static TabId tabIdFromDiagnosticName(const char *) noexcept { return TabId::Other; }
    static ActionId actionIdFromAction(Action) noexcept { return ActionId::Other; }
    inline void setScreen(ScreenId) noexcept {}
    inline void setTab(TabId) noexcept {}
    inline void setAction(ActionId) noexcept {}
    inline void setPlaybackState(PlaybackState) noexcept {}
    inline void setWorkerActive(WorkerId, bool) noexcept {}
    inline void setWorkerQueueDepth(WorkerId, uint32_t) noexcept {}
    inline void addWorkerCompleted(WorkerId, uint32_t = 1) noexcept {}
    inline void addWorkerFailed(WorkerId, uint32_t = 1) noexcept {}
    inline void addWorkerCancelled(WorkerId, uint32_t = 1) noexcept {}
    inline void recordArtworkCacheProbe(bool) noexcept {}
    inline void recordArtworkCacheRead(bool, uint64_t) noexcept {}
    inline void recordArtworkCacheWrite(bool, uint64_t) noexcept {}
    inline void recordArtworkDecode(bool, uint64_t) noexcept {}
    inline void recordFramePhase(FramePhase, uint64_t) noexcept {}
    inline void setDownloadGauges(uint32_t, uint32_t, uint32_t) noexcept {}
    inline void addDownloadBytes(uint64_t) noexcept {}
    inline void addDownloadSegmentCompleted(uint32_t = 1) noexcept {}
    inline void addDownloadSegmentRetries(uint32_t = 1) noexcept {}
    inline void setCatalogDbActive(bool) noexcept {}
    inline void setCatalogDbQueueDepth(uint32_t) noexcept {}
    inline void addCatalogDbCompleted(uint32_t = 1) noexcept {}
    inline void addCatalogDbFailed(uint32_t = 1) noexcept {}
    inline void addCatalogDbCancelled(uint32_t = 1) noexcept {}
    inline void recordCatalogDbQuery(uint64_t) noexcept {}
    inline void recordCatalogDbTransaction(uint64_t) noexcept {}
    inline void recordCatalogDbCommit(uint64_t) noexcept {}
    inline void recordCatalogDbQueueWait(uint64_t) noexcept {}
    inline void addCatalogDbEnqueueRejected(uint32_t = 1) noexcept {}
    inline void addCatalogDbRows(uint32_t, uint32_t, uint32_t) noexcept {}
    inline void recordCatalogDbSqliteError(int) noexcept {}
};

inline PerformanceTelemetry &performanceTelemetry() noexcept
{
    static PerformanceTelemetry instance;
    return instance;
}

#endif

} // namespace miyoofin

#endif // MIYOOFIN_PERFORMANCE_TELEMETRY_HPP
