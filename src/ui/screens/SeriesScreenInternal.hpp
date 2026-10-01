#pragma once

#include <string>
#include <vector>

namespace miyoofin {

// Layout on the 640x480 framebuffer (below the 44px header, above the 24px footer):
// show poster + bio on the left, a 3x2 grid of season cards on the right.
inline constexpr int FB_W = 640, FB_H = 480, BOTTOM_H = 24;
inline constexpr int SHOW_X = 16, SHOW_Y = 56, SHOW_W = 120, SHOW_H = 180;
inline constexpr int META_X = 16, META_WRAP = 33; // bio column: 264px = 33 glyphs
inline constexpr int OVERVIEW_Y = 258, OVERVIEW_PITCH = 18;
inline constexpr int GRID_COLS = 3, GRID_ROWS = 2, GRID_VISIBLE = GRID_COLS * GRID_ROWS;
inline constexpr int GRID_HEAD_Y = 56, GRID_TOP_Y = 84, GRID_ROW_H = 176;
inline constexpr int POSTER_W = 100, POSTER_H = 150;
inline constexpr int COL_X[GRID_COLS] = {300, 410, 520};
inline int overviewVisibleLines()
{
    const int lines = (FB_H - BOTTOM_H - 8 - OVERVIEW_Y) / OVERVIEW_PITCH;
    return lines < 1 ? 1 : lines;
}
inline int gridRowCount(int totalSeasons)
{
    return (totalSeasons + GRID_COLS - 1) / GRID_COLS;
}

inline std::vector<std::string> wrapOverview(const char* text, int wrapCols)
{
    std::vector<std::string> lines;
    if (!text || !*text)
        return lines;
    std::string input(text);
    size_t pos = 0;
    while (pos < input.size()) {
        size_t newline = input.find('\n', pos);
        std::string para =
            (newline != std::string::npos) ? input.substr(pos, newline - pos) : input.substr(pos);
        while (!para.empty()) {
            if ((int)para.size() <= wrapCols) {
                lines.push_back(para);
                break;
            }
            size_t lastSpace = para.rfind(' ', wrapCols);
            if (lastSpace != std::string::npos && lastSpace > 0) {
                lines.push_back(para.substr(0, lastSpace));
                para = para.substr(lastSpace + 1);
            } else {
                lines.push_back(para.substr(0, wrapCols));
                para = para.substr(wrapCols);
            }
        }
        if (newline != std::string::npos)
            pos = newline + 1;
        else
            break;
    }
    return lines;
}
}
