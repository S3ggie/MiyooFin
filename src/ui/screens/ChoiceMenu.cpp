#include "ChoiceMenu.hpp"
#include "../UiKit.hpp"
#include <algorithm>

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
    const int visible = count < kMaxVisible ? count : kMaxVisible;
    const int first = std::max(0, std::min(m_selected - visible / 2, count - visible));
    const int w = 380, h = 56 + visible * 32;
    const int x = (d::kScreenW - w) / 2, y = (d::kScreenH - h) / 2;
    ui::blend(fb, 0, 0, d::kScreenW, d::kScreenH, d::kCanvas, 190);
    ui::panel(fb, x, y, w, h);
    ui::textClamped(fb, x + 16, y + 14, w - 32, m_title, d::kText);
    for (int i = 0; i < visible; ++i) {
        const int index = first + i;
        const int ry = y + 44 + i * 32;
        const bool sel = index == m_selected;
        if (sel)
            ui::focusRing(fb, x + 10, ry, w - 20, 28);
        ui::roundFill(fb, x + 10, ry, w - 20, 28, d::kRadius, sel ? d::kRaised : d::kPanel);
        ui::textClamped(fb, x + 22, ry + 6, w - 44, m_items[index],
                        sel ? d::kAccentHi : d::kTextSecondary);
    }
    if (count > visible) {
        const int trackH = visible * 32 - 4;
        const int thumbH = std::max(12, trackH * visible / count);
        const int thumbY = y + 44 + (trackH - thumbH) * first / std::max(1, count - visible);
        ui::fill(fb, x + w - 8, y + 44, 3, trackH, d::kDivider);
        ui::roundFill(fb, x + w - 8, thumbY, 3, thumbH, 1, d::kAccentDim);
    }
}

} // namespace miyoofin
