#ifndef MIYOOFIN_AUDIO_CHOICE_MENU_HPP
#define MIYOOFIN_AUDIO_CHOICE_MENU_HPP

#include "../../download/DownloadAudio.hpp"
#include "../../input/Action.hpp"
#include <SDL2/SDL.h>
#include <vector>

namespace miyoofin {

/// Modal "which audio track?" list shown when a download is confirmed.
/// Owned by a screen; holds the pending items until a track is picked.
class AudioChoiceMenu
{
  public:
    enum class Result
    {
        None,
        Picked,
        Cancelled
    };
    /// Opens the menu. False (and nothing opens) when the items offer no audio
    /// language to choose from, so the caller enqueues them as they are.
    bool begin(const std::vector<DownloadItem>& items);
    bool active() const
    {
        return m_active;
    }
    Result handle(Action action);
    /// Items with the picked language applied; valid after Result::Picked.
    std::vector<DownloadItem> takeItems();
    void render(SDL_Surface* fb) const;

  private:
    bool m_active = false;
    int m_selected = 0;
    std::vector<DownloadAudioOption> m_options;
    std::vector<DownloadItem> m_items;
};

} // namespace miyoofin
#endif
