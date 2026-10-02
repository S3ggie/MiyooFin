#include "AudioChoiceMenu.hpp"
#include "../UiKit.hpp"
#include <algorithm>

namespace miyoofin {

bool AudioChoiceMenu::begin(const std::vector<DownloadItem>& items)
{
    m_options = downloadAudioOptions(items);
    if (m_options.size() < 2)
        return false;
    m_items = items;
    m_selected = 0;
    m_active = true;
    return true;
}

AudioChoiceMenu::Result AudioChoiceMenu::handle(Action action)
{
    const int count = (int)m_options.size();
    switch (action) {
    case Action::Up:
        m_selected = (m_selected + count - 1) % count;
        break;
    case Action::Down:
        m_selected = (m_selected + 1) % count;
        break;
    case Action::Confirm:
        applyDownloadAudio(m_items, m_options[m_selected].lang);
        m_active = false;
        return Result::Picked;
    case Action::Back:
        m_active = false;
        m_items.clear();
        return Result::Cancelled;
    default:
        break;
    }
    return Result::None;
}

std::vector<DownloadItem> AudioChoiceMenu::takeItems()
{
    return std::move(m_items);
}

void AudioChoiceMenu::render(SDL_Surface* fb) const
{
    if (!m_active)
        return;
    namespace d = design;
    constexpr int rowH = 30, maxRows = 8;
    const int visible = std::min((int)m_options.size(), maxRows);
    const int first = std::min(std::max(0, m_selected - visible + 1 + visible / 2),
                               (int)m_options.size() - visible);
    const int w = 340, h = 76 + visible * rowH;
    const int x = (d::kScreenW - w) / 2, y = (d::kScreenH - h) / 2;
    ui::blend(fb, 0, 0, d::kScreenW, d::kScreenH, d::kCanvas, 190);
    ui::panel(fb, x, y, w, h);
    ui::text(fb, x + 16, y + 14, "Download audio", d::kText);
    for (int i = 0; i < visible; ++i) {
        const int idx = first + i;
        const int ry = y + 44 + i * rowH;
        const bool sel = idx == m_selected;
        if (sel)
            ui::focusRing(fb, x + 10, ry, w - 20, rowH - 4);
        ui::roundFill(fb, x + 10, ry, w - 20, rowH - 4, d::kRadius, sel ? d::kRaised : d::kPanel);
        ui::textClamped(fb, x + 22, ry + 6, w - 44, m_options[idx].label,
                        sel ? d::kText : d::kTextSecondary);
    }
    ui::text(fb, x + 16, y + h - 24, "A Download   B Cancel", d::kTextSecondary);
}

} // namespace miyoofin
