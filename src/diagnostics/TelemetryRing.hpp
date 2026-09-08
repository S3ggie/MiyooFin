#ifndef MIYOOFIN_TELEMETRY_RING_HPP
#define MIYOOFIN_TELEMETRY_RING_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "TelemetryTypes.hpp"

namespace miyoofin {

template <std::size_t Capacity>
class TelemetryRing
{
    // Sequence arithmetic relies on producer-consumer distance staying far
    // below 2^31; capacity 512 and bounded producer contention preserve that.
    static_assert(Capacity != 0 && (Capacity & (Capacity - 1)) == 0,
        "TelemetryRing capacity must be a nonzero power of two");
    static_assert(Capacity < (std::size_t{1} << 31),
        "TelemetryRing capacity must be less than 2^31");

    struct Slot
    {
        std::atomic<uint32_t> sequence{0};
        TelemetryRecord record{};
    };

public:
    TelemetryRing()
    {
        for (std::size_t index = 0; index < Capacity; ++index)
            m_slots[index].sequence.store(static_cast<uint32_t>(index), std::memory_order_relaxed);
    }

    bool tryEnqueue(const TelemetryRecord &record) noexcept
    {
        uint32_t position = m_enqueuePos.load(std::memory_order_relaxed);
        for (unsigned int reservationAttempt = 0; reservationAttempt < 8; ++reservationAttempt) {
            Slot &slot = m_slots[position & static_cast<uint32_t>(Capacity - 1)];
            const uint32_t sequence = slot.sequence.load(std::memory_order_acquire);
            const int32_t difference = static_cast<int32_t>(sequence - position);
            if (difference < 0)
                return false;
            if (difference > 0) {
                position = m_enqueuePos.load(std::memory_order_relaxed);
                continue;
            }
            if (!m_enqueuePos.compare_exchange_weak(position, position + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                continue;

            // The producer's record copy becomes visible only with the release
            // publication below. Position CAS only arbitrates producer tickets.
            slot.record = record;
            const uint32_t depth = m_approximateDepth.fetch_add(1,
                std::memory_order_relaxed) + 1;
            updateHighwater(depth);
            slot.sequence.store(position + 1, std::memory_order_release);
            return true;
        }
        return false;
    }

    bool tryDequeue(TelemetryRecord &record) noexcept
    {
        const uint32_t position = m_dequeuePos;
        Slot &slot = m_slots[position & static_cast<uint32_t>(Capacity - 1)];
        const uint32_t sequence = slot.sequence.load(std::memory_order_acquire);
        const int32_t difference = static_cast<int32_t>(sequence - (position + 1));
        if (difference != 0)
            return false;

        // The consumer acquire sees the producer's release publication. Its
        // release below exposes slot reuse to a future producer acquire.
        record = slot.record;
        slot.sequence.store(position + static_cast<uint32_t>(Capacity),
            std::memory_order_release);
        m_approximateDepth.fetch_sub(1, std::memory_order_relaxed);
        ++m_dequeuePos;
        return true;
    }

    uint32_t approximateDepth() const noexcept
    {
        return m_approximateDepth.load(std::memory_order_relaxed);
    }

    uint32_t approximateHighWatermark() const noexcept
    {
        return m_approximateHighwater.load(std::memory_order_relaxed);
    }

#if defined(MIYOOFIN_TELEMETRY_RING_TEST)
    void seedPositionsForTest(uint32_t position) noexcept
    {
        const uint32_t firstSlot = position & static_cast<uint32_t>(Capacity - 1);
        for (std::size_t index = 0; index < Capacity; ++index) {
            const uint32_t offset = (static_cast<uint32_t>(index) +
                static_cast<uint32_t>(Capacity) - firstSlot) &
                static_cast<uint32_t>(Capacity - 1);
            m_slots[index].sequence.store(position + offset,
                std::memory_order_relaxed);
        }
        m_enqueuePos.store(position, std::memory_order_relaxed);
        m_dequeuePos = position;
        m_approximateDepth.store(0, std::memory_order_relaxed);
        m_approximateHighwater.store(0, std::memory_order_relaxed);
    }
#endif

private:
    void updateHighwater(uint32_t depth) noexcept
    {
        uint32_t observed = m_approximateHighwater.load(std::memory_order_relaxed);
        for (unsigned int attempt = 0; attempt < 8 && depth > observed; ++attempt) {
            if (m_approximateHighwater.compare_exchange_weak(observed, depth,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                return;
        }
    }

    std::array<Slot, Capacity> m_slots{};
    std::atomic<uint32_t> m_enqueuePos{0};
    uint32_t m_dequeuePos = 0;
    std::atomic<uint32_t> m_approximateDepth{0};
    std::atomic<uint32_t> m_approximateHighwater{0};
};

} // namespace miyoofin

#endif // MIYOOFIN_TELEMETRY_RING_HPP
