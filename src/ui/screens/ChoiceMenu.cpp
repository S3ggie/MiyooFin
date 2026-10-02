#include "ChoiceMenu.hpp"
#include "../UiKit.hpp"

namespace miyoofin {

void ChoiceMenu::open(std::string title, std::vector<std::string> items)
{
    m_title = std::move(title);
    m_items = std::move(items);
    m_selected = 0;
    m_active = !m_items.empty();
}

ChoiceMenu::Result ChoiceMenu::handle(Action action)
{
    const int count = static_cast<int>(m_items.size());
    switch (action) {
    case Action::Up:
        m_selected = (m_selected + count - 1) % count;
        break;
    case Action::Down:
        m_selected = (m_selected + 1) % count;
        break;
    case Action::Confirm:
        m_active = false;
        return Result::Chosen;
    case Action::Back:
    case Action::Menu:
        m_active = false;
        return Result::Cancelled;
    default:
        break;
    }
    return Result::None;
}

void ChoiceMenu::render(SDL_Surface* fb) const
{
    if (!m_active)
        return;
    namespace d = design;
    const int count = static_cast<int>(m_items.size());
    const int w = 360, h = 56 + count * 32;
    const int x = (d::kScreenW - w) / 2, y = (d::kScreenH - h) / 2;
    ui::blend(fb, 0, 0, d::kScreenW, d::kScreenH, d::kCanvas, 190);
    ui::panel(fb, x, y, w, h);
    ui::textClamped(fb, x + 16, y + 14, w - 32, m_title, d::kText);
    for (int i = 0; i < count; ++i) {
        const int ry = y + 44 + i * 32;
        const bool sel = i == m_selected;
        if (sel)
            ui::focusRing(fb, x + 10, ry, w - 20, 28);
        ui::roundFill(fb, x + 10, ry, w - 20, 28, d::kRadius, sel ? d::kRaised : d::kPanel);
        ui::textClamped(fb, x + 22, ry + 6, w - 44, m_items[i],
                        sel ? d::kAccentHi : d::kTextSecondary);
    }
}

} // namespace miyoofin
