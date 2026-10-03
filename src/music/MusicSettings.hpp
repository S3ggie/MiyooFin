#ifndef MIYOOFIN_MUSIC_SETTINGS_HPP
#define MIYOOFIN_MUSIC_SETTINGS_HPP

#include <atomic>
#include <string>

namespace miyoofin {
namespace music {

/// Options set in the Music Settings tab. Shared between the UI thread (changes them) and
/// the fetch/download workers (read them), hence atomics.
struct MusicSettings
{
    std::atomic<int> streamKbps{192};
    std::atomic<int> downloadKbps{192};
    std::atomic<bool> albumGrid{false}; ///< Albums tab as a cover grid instead of a list

    /// Cycles 128 -> 192 -> 320 -> 128.
    static int nextKbps(int kbps)
    {
        return kbps < 192 ? 192 : (kbps < 320 ? 320 : 128);
    }
    static int sanitize(int kbps)
    {
        return kbps == 128 || kbps == 192 || kbps == 320 ? kbps : 192;
    }
    void load(const std::string& path);
    bool save(const std::string& path) const;
};

} // namespace music
} // namespace miyoofin

#endif
