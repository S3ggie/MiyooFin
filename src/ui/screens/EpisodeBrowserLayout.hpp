#pragma once

namespace miyoofin {

// Episode browser geometry on the 640x480 framebuffer: header (44px) and
// footer (24px) from the shared kit, episode list on the left, the selected
// episode's thumbnail, facts, bio and actions on the right. Shared by drawing,
// scrolling and artwork prefetch so they cannot drift apart.
inline constexpr int EB_FB_H = 480;
inline constexpr int EB_LIST_X = 16, EB_LIST_W = 284, EB_LIST_Y = 56;
inline constexpr int EB_ROW_H = 38, EB_ROW_PITCH = 42;
inline constexpr int EB_THUMB_X = 326, EB_THUMB_Y = 56, EB_THUMB_W = 288, EB_THUMB_H = 162;
inline constexpr int EB_RIGHT_X = 316, EB_RIGHT_W = 308;
inline constexpr int EB_TITLE_Y = EB_THUMB_Y + EB_THUMB_H + 12;
inline constexpr int EB_CHIPS_Y = EB_TITLE_Y + 24;
inline constexpr int EB_OVERVIEW_Y = EB_CHIPS_Y + 30, EB_OVERVIEW_PITCH = 18;
inline constexpr int EB_BTN_H = 34, EB_BTN_Y = EB_FB_H - 24 - 8 - EB_BTN_H;
inline constexpr int EB_META_WRAP = 37; // glyph columns of the bio text
inline constexpr int EB_LIST_VISIBLE = 9;

inline int episodeOverviewVisibleLines()
{
    const int lines = (EB_BTN_Y - 26 - EB_OVERVIEW_Y) / EB_OVERVIEW_PITCH;
    return lines < 1 ? 1 : lines;
}

} // namespace miyoofin
