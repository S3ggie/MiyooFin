#include "PerformanceTelemetry.hpp"
#include "PerformanceTelemetryInternal.hpp"

namespace miyoofin {
using namespace telemetry_internal;

bool PerformanceTelemetry::enabledFast() const noexcept
{
    return m_enabled.load(std::memory_order_relaxed);
}
void PerformanceTelemetry::emitStateTransition(StateKind kind,
                                               uint16_t previous,
                                               uint16_t current) noexcept
{
    TelemetryRecord record{};
    record.header.record_type = RecordType::StateTransition;
    record.header.monotonic_us = monotonicUs();
    record.payload.state_transition.state_kind = static_cast<uint8_t>(kind);
    record.payload.state_transition.previous_id = previous;
    record.payload.state_transition.current_id = current;
    record.payload.state_transition.transition_seq =
        m_transitionSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    enqueue(record);
}
void PerformanceTelemetry::setScreen(ScreenId screen) noexcept
{
    if (!enabledFast())
        return;
    const uint16_t current = static_cast<uint16_t>(screen);
    const uint16_t previous = m_screenId.exchange(current, std::memory_order_relaxed);
    if (previous != current)
        emitStateTransition(StateKind::Screen, previous, current);
}
void PerformanceTelemetry::setTab(TabId tab) noexcept
{
    if (!enabledFast())
        return;
    const uint16_t current = static_cast<uint16_t>(tab);
    const uint16_t previous = m_tabId.exchange(current, std::memory_order_relaxed);
    if (previous != current)
        emitStateTransition(StateKind::Tab, previous, current);
}
void PerformanceTelemetry::setAction(ActionId action) noexcept
{
    if (!enabledFast())
        return;
    const uint16_t current = static_cast<uint16_t>(action);
    const uint16_t previous = m_actionId.exchange(current, std::memory_order_relaxed);
    if (previous != current)
        emitStateTransition(StateKind::Action, previous, current);
}
void PerformanceTelemetry::setPlaybackState(PlaybackState state) noexcept
{
    if (!enabledFast())
        return;
    const uint16_t current = static_cast<uint16_t>(state);
    const uint16_t previous = m_playbackState.exchange(current, std::memory_order_relaxed);
    if (previous != current)
        emitStateTransition(StateKind::PlaybackState, previous, current);
}
PerformanceTelemetry::WorkerSlot *PerformanceTelemetry::workerSlot(WorkerId worker) noexcept
{
    const uint16_t value = static_cast<uint16_t>(worker);
    if (value == 0 || value > m_workerSlots.size())
        return nullptr;
    return &m_workerSlots[value - 1];
}
void PerformanceTelemetry::setWorkerActive(WorkerId worker, bool active) noexcept
{
    if (!enabledFast())
        return;
    if (WorkerSlot *slot = workerSlot(worker))
        slot->active.store(active ? 1u : 0u, std::memory_order_relaxed);
}
void PerformanceTelemetry::updateQueueHighwater(uint32_t depth) noexcept
{
    uint32_t observed = m_queueHighwater.load(std::memory_order_relaxed);
    while (depth > observed
        && !m_queueHighwater.compare_exchange_weak(observed, depth,
                                                    std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
    }
}
void PerformanceTelemetry::setWorkerQueueDepth(WorkerId worker, uint32_t depth) noexcept
{
    if (!enabledFast())
        return;
    if (WorkerSlot *slot = workerSlot(worker)) {
        slot->queueDepth.store(depth, std::memory_order_relaxed);
        uint32_t observed = slot->queueHighwater.load(std::memory_order_relaxed);
        while (depth > observed
            && !slot->queueHighwater.compare_exchange_weak(observed, depth,
                                                            std::memory_order_relaxed,
                                                            std::memory_order_relaxed)) {
        }
    }
}
void PerformanceTelemetry::addWorkerCompleted(WorkerId worker, uint32_t count) noexcept
{
    if (!enabledFast())
        return;
    if (WorkerSlot *slot = workerSlot(worker))
        slot->completed.fetch_add(count, std::memory_order_relaxed);
}
void PerformanceTelemetry::addWorkerFailed(WorkerId worker, uint32_t count) noexcept
{
    if (!enabledFast())
        return;
    if (WorkerSlot *slot = workerSlot(worker))
        slot->failed.fetch_add(count, std::memory_order_relaxed);
}
void PerformanceTelemetry::addWorkerCancelled(WorkerId worker, uint32_t count) noexcept
{
    if (!enabledFast())
        return;
    if (WorkerSlot *slot = workerSlot(worker))
        slot->cancelled.fetch_add(count, std::memory_order_relaxed);
}
void PerformanceTelemetry::recordArtworkCacheProbe(bool hit) noexcept
{
    if (!enabledFast())
        return;
    (hit ? m_artworkCacheProbeHits : m_artworkCacheProbeMisses)
        .fetch_add(1, std::memory_order_relaxed);
}
void PerformanceTelemetry::recordArtworkCacheRead(bool success, uint64_t compressedBytes) noexcept
{
    if (!enabledFast())
        return;
    if (success) {
        m_artworkCacheReadSuccess.fetch_add(1, std::memory_order_relaxed);
        m_artworkCompressedReadBytes.fetch_add(compressedBytes, std::memory_order_relaxed);
    } else {
        m_artworkCacheReadFailure.fetch_add(1, std::memory_order_relaxed);
    }
}
void PerformanceTelemetry::recordArtworkCacheWrite(bool success, uint64_t compressedBytes) noexcept
{
    if (!enabledFast())
        return;
    if (success) {
        m_artworkCacheWriteSuccess.fetch_add(1, std::memory_order_relaxed);
        m_artworkCompressedWrittenBytes.fetch_add(compressedBytes, std::memory_order_relaxed);
    } else {
        m_artworkCacheWriteFailure.fetch_add(1, std::memory_order_relaxed);
    }
}
void PerformanceTelemetry::recordArtworkDecode(bool success, uint64_t durationUs) noexcept
{
    if (!enabledFast())
        return;
    m_artworkDecodeCount.fetch_add(1, std::memory_order_relaxed);
    if (!success)
        m_artworkDecodeFailures.fetch_add(1, std::memory_order_relaxed);
    m_artworkDecodeTotalUs.fetch_add(durationUs, std::memory_order_relaxed);
    uint32_t observed = m_artworkDecodeMaxUs.load(std::memory_order_relaxed);
    const uint32_t duration = clampToUint32(durationUs);
    while (duration > observed
        && !m_artworkDecodeMaxUs.compare_exchange_weak(observed, duration,
                                                        std::memory_order_relaxed,
                                                        std::memory_order_relaxed)) {
    }
}
void PerformanceTelemetry::recordFramePhase(FramePhase phase, uint64_t durationUs) noexcept
{
    if (!enabledFast())
        return;
    const uint8_t phaseValue = static_cast<uint8_t>(phase);
    if (phaseValue < static_cast<uint8_t>(FramePhase::FullFrame)
        || phaseValue > static_cast<uint8_t>(FramePhase::Present))
        return;

    FramePhaseAccumulator &accumulator = m_framePhases[phaseValue - 1];
    accumulator.count.fetch_add(1, std::memory_order_relaxed);
    accumulator.totalUs.fetch_add(durationUs, std::memory_order_relaxed);
    accumulator.over50Ms.fetch_add(durationUs > 50000 ? 1u : 0u, std::memory_order_relaxed);
    accumulator.over100Ms.fetch_add(durationUs > 100000 ? 1u : 0u, std::memory_order_relaxed);
    const uint32_t duration = clampToUint32(durationUs);
    uint32_t observed = accumulator.maxUs.load(std::memory_order_relaxed);
    while (duration > observed
        && !accumulator.maxUs.compare_exchange_weak(observed, duration,
                                                    std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
    }

    std::size_t histogramBin = 8;
    if (durationUs <= 8333)
        histogramBin = 0;
    else if (durationUs <= 16667)
        histogramBin = 1;
    else if (durationUs <= 25000)
        histogramBin = 2;
    else if (durationUs <= 33333)
        histogramBin = 3;
    else if (durationUs <= 50000)
        histogramBin = 4;
    else if (durationUs <= 100000)
        histogramBin = 5;
    else if (durationUs <= 250000)
        histogramBin = 6;
    else if (durationUs <= 500000)
        histogramBin = 7;
    accumulator.histogram[histogramBin].fetch_add(1, std::memory_order_relaxed);
}
void PerformanceTelemetry::setDownloadGauges(uint32_t activeDownloads,
                                             uint32_t queuedDownloads,
                                             uint32_t plannerQueueDepth) noexcept
{
    if (!enabledFast())
        return;
    m_activeDownloads.store(activeDownloads, std::memory_order_relaxed);
    m_queuedDownloads.store(queuedDownloads, std::memory_order_relaxed);
    m_plannerQueueDepth.store(plannerQueueDepth, std::memory_order_relaxed);
}
void PerformanceTelemetry::addDownloadBytes(uint64_t bytes) noexcept
{
    if (enabledFast())
        m_downloadBytes.fetch_add(bytes, std::memory_order_relaxed);
}
void PerformanceTelemetry::addDownloadSegmentCompleted(uint32_t count) noexcept
{
    if (enabledFast())
        m_downloadSegmentsCompleted.fetch_add(count, std::memory_order_relaxed);
}
void PerformanceTelemetry::addDownloadSegmentRetries(uint32_t count) noexcept
{
    if (enabledFast())
        m_downloadSegmentRetries.fetch_add(count, std::memory_order_relaxed);
}
void PerformanceTelemetry::setCatalogDbActive(bool active) noexcept
{
    if (enabledFast())
        m_catalogDbActive.store(active ? 1u : 0u, std::memory_order_relaxed);
}
void PerformanceTelemetry::updateCatalogDbQueueHighwater(uint32_t depth) noexcept
{
    uint32_t observed = m_catalogDbQueueHighwater.load(std::memory_order_relaxed);
    while (depth > observed
        && !m_catalogDbQueueHighwater.compare_exchange_weak(
               observed, depth, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}
void PerformanceTelemetry::setCatalogDbQueueDepth(uint32_t depth) noexcept
{
    if (!enabledFast())
        return;
    m_catalogDbQueueDepth.store(depth, std::memory_order_relaxed);
    updateCatalogDbQueueHighwater(depth);
}
void PerformanceTelemetry::addCatalogDbCompleted(uint32_t count) noexcept
{
    if (enabledFast())
        m_catalogDbCompleted.fetch_add(count, std::memory_order_relaxed);
}
void PerformanceTelemetry::addCatalogDbFailed(uint32_t count) noexcept
{
    if (enabledFast())
        m_catalogDbFailed.fetch_add(count, std::memory_order_relaxed);
}
void PerformanceTelemetry::addCatalogDbCancelled(uint32_t count) noexcept
{
    if (enabledFast())
        m_catalogDbCancelled.fetch_add(count, std::memory_order_relaxed);
}
void PerformanceTelemetry::recordCatalogDbQuery(uint64_t durationUs) noexcept
{
    if (!enabledFast())
        return;
    m_catalogDbQueryCount.fetch_add(1, std::memory_order_relaxed);
    m_catalogDbQueryTotalUs.fetch_add(durationUs, std::memory_order_relaxed);
    updateMaximum(m_catalogDbQueryMaxUs, durationUs);
}
void PerformanceTelemetry::recordCatalogDbTransaction(uint64_t durationUs) noexcept
{
    if (!enabledFast())
        return;
    m_catalogDbTransactionCount.fetch_add(1, std::memory_order_relaxed);
    m_catalogDbTransactionTotalUs.fetch_add(durationUs, std::memory_order_relaxed);
    updateMaximum(m_catalogDbTransactionMaxUs, durationUs);
}
void PerformanceTelemetry::recordCatalogDbCommit(uint64_t durationUs) noexcept
{
    if (!enabledFast())
        return;
    m_catalogDbCommitCount.fetch_add(1, std::memory_order_relaxed);
    m_catalogDbCommitTotalUs.fetch_add(durationUs, std::memory_order_relaxed);
    updateMaximum(m_catalogDbCommitMaxUs, durationUs);
}
void PerformanceTelemetry::recordCatalogDbQueueWait(uint64_t durationUs) noexcept
{
    if (!enabledFast())
        return;
    m_catalogDbQueueWaitTotalUs.fetch_add(durationUs, std::memory_order_relaxed);
    updateMaximum(m_catalogDbQueueWaitMaxUs, durationUs);
}
void PerformanceTelemetry::addCatalogDbEnqueueRejected(uint32_t count) noexcept
{
    if (enabledFast())
        m_catalogDbEnqueueRejected.fetch_add(count, std::memory_order_relaxed);
}
void PerformanceTelemetry::addCatalogDbRows(uint32_t inserted, uint32_t updated,
                                            uint32_t deleted) noexcept
{
    if (!enabledFast())
        return;
    m_catalogDbRowsInserted.fetch_add(inserted, std::memory_order_relaxed);
    m_catalogDbRowsUpdated.fetch_add(updated, std::memory_order_relaxed);
    m_catalogDbRowsDeleted.fetch_add(deleted, std::memory_order_relaxed);
}
void PerformanceTelemetry::recordCatalogDbSqliteError(int resultCode) noexcept
{
    if (!enabledFast())
        return;
    const int primary = resultCode & 0xff;
    if (primary == 5) {
        m_catalogDbBusyFamily.fetch_add(1, std::memory_order_relaxed);
    } else if (primary == 10) {
        m_catalogDbIoerrFamily.fetch_add(1, std::memory_order_relaxed);
    } else if (primary == 11 || primary == 26) {
        m_catalogDbCorruptNotadb.fetch_add(1, std::memory_order_relaxed);
    }
}
void PerformanceTelemetry::suspendSampling(bool suspended, SamplingReason reason) noexcept
{
    (void)reason;
    if (enabledFast())
        m_samplingSuspended.store(suspended, std::memory_order_relaxed);
}
uint64_t PerformanceTelemetry::nextEphemeralId() noexcept
{
    if (!enabledFast())
        return 0;
    return m_nextEphemeralId.fetch_add(1, std::memory_order_relaxed);
}
bool PerformanceTelemetry::emitRecord(const TelemetryRecord &record) noexcept
{
    if (!enabledFast())
        return false;
    TelemetryRecord copy = record;
    if (copy.header.monotonic_us == 0)
        copy.header.monotonic_us = monotonicUs();
    return enqueue(copy);
}
bool PerformanceTelemetry::emitSessionEvent(SessionEventKind kind,
                                             Outcome outcome,
                                             uint32_t value0,
                                             uint64_t value1) noexcept
{
    if (!enabledFast())
        return false;
    TelemetryRecord record{};
    record.header.record_type = RecordType::SessionEvent;
    record.header.monotonic_us = monotonicUs();
    record.payload.session_event.kind = static_cast<uint8_t>(kind);
    record.payload.session_event.outcome = static_cast<uint8_t>(outcome);
    record.payload.session_event.value0 = value0;
    record.payload.session_event.value1 = value1;
    return enqueue(record);
}
bool PerformanceTelemetry::enqueue(TelemetryRecord record) noexcept
{
    if (!m_ring.tryEnqueue(record)) {
        m_droppedRecords.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    updateQueueHighwater(m_ring.approximateDepth());
    return true;
}
}
