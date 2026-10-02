#ifndef MIYOOFIN_CHOICE_MENU_HPP
#define MIYOOFIN_CHOICE_MENU_HPP

#include "../../input/Action.hpp"
#include <SDL2/SDL.h>
#include <string>
#include <vector>

namespace miyoofin {

/// A small modal list ("Mark episode watched" / "Mark season watched"): Up/Down to pick,
/// A to choose, B to close. The owning screen routes input to it while it is open.
class ChoiceMenu
{
  public:
    enum class Result
    {
        None,
        Chosen,
        Cancelled
    };
    void open(std::string title, std::vector<std::string> items);
    /// Keeps the cursor where it was (a list that is rebuilt while open).
    int selected() const
    {
        return m_selected;
    }
    bool active() const
    {
        return m_active;
    }
    Result handle(Action action);
    /// Index of the item picked by the last `Chosen` result.
    int chosen() const
    {
        return m_selected;
    }
    void render(SDL_Surface* fb) const;

  private:
    static constexpr int kMaxVisible = 8;
    bool m_active = false;
    int m_selected = 0;
    std::string m_title;
    std::vector<std::string> m_items;
};

} // namespace miyoofin

#endif
