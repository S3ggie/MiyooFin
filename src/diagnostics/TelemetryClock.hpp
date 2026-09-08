#pragma once

#include <cstdint>
#include <ctime>

namespace miyoofin {

class TelemetryClock
{
public:
    static uint64_t monotonicUs() noexcept
    {
        timespec timestamp{};
        if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0)
            return 0;

        return static_cast<uint64_t>(timestamp.tv_sec) * 1000000ull
            + static_cast<uint64_t>(timestamp.tv_nsec) / 1000ull;
    }

    static uint64_t processCpuUs() noexcept
    {
        timespec timestamp{};
        if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &timestamp) != 0)
            return 0;

        return static_cast<uint64_t>(timestamp.tv_sec) * 1000000ull
            + static_cast<uint64_t>(timestamp.tv_nsec) / 1000ull;
    }
};

} // namespace miyoofin
