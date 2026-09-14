#ifndef MIYOOFIN_HLS_PROFILE_HPP
#define MIYOOFIN_HLS_PROFILE_HPP

#include <cstdint>
#include <limits>

namespace miyoofin {
constexpr std::uint64_t HLS_VIDEO_BITRATE = 1200000;
constexpr std::uint64_t HLS_AUDIO_BITRATE = 96000;
constexpr unsigned HLS_MAX_WIDTH = 640;
constexpr unsigned HLS_MAX_HEIGHT = 480;
constexpr unsigned HLS_MAX_FRAMERATE = 30;
constexpr std::uint64_t HLS_TOTAL_BITRATE = HLS_VIDEO_BITRATE + HLS_AUDIO_BITRATE;
constexpr std::uint64_t HLS_TICKS_PER_SECOND = 10000000;
constexpr std::uint64_t HLS_OVERHEAD_DIVISOR = 20; // 5% playlist/container headroom
constexpr std::uint64_t HLS_FIXED_OVERHEAD_BYTES = 64 * 1024;
constexpr const char *HLS_PROFILE_NAME = "h264-aac-640x480-1200000";

// Returns false when runtime is unavailable or the conservative estimate
// cannot be represented.  The calculation rounds partial seconds up.
inline bool estimateHlsBytes(std::int64_t runtimeTicks, std::uint64_t &bytes) {
    bytes = 0;
    if (runtimeTicks <= 0) return false;
    const std::uint64_t ticks = static_cast<std::uint64_t>(runtimeTicks);
    const std::uint64_t seconds = ticks / HLS_TICKS_PER_SECOND;
    const std::uint64_t remainder = ticks % HLS_TICKS_PER_SECOND;
    const std::uint64_t bytesPerSecond = HLS_TOTAL_BITRATE / 8;
    if (seconds > (std::numeric_limits<std::uint64_t>::max)() / bytesPerSecond) return false;
    bytes = seconds * bytesPerSecond;
    const std::uint64_t partial = (remainder * HLS_TOTAL_BITRATE + HLS_TICKS_PER_SECOND * 8 - 1) /
                                  (HLS_TICKS_PER_SECOND * 8);
    if (bytes > (std::numeric_limits<std::uint64_t>::max)() - partial) return false;
    bytes += partial;
    const std::uint64_t overhead = bytes / HLS_OVERHEAD_DIVISOR;
    if (bytes > (std::numeric_limits<std::uint64_t>::max)() - overhead ||
        bytes + overhead > (std::numeric_limits<std::uint64_t>::max)() - HLS_FIXED_OVERHEAD_BYTES) return false;
    bytes += overhead + HLS_FIXED_OVERHEAD_BYTES;
    return true;
}
}
#endif
