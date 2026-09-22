#pragma once

#include <string>
#include <vector>

namespace miyoofin {

inline constexpr int FB_H = 480;
inline constexpr int BOTTOM_H = 18;
inline constexpr int META_Y = 305;
inline constexpr int META_WRAP = 24;
inline constexpr int GRID_COLS = 2;
inline constexpr int FB_W = 640, HEAD_X = 22, HEAD_Y = 16, GRID_TOP_Y = 51, GRID_ROW_H = 135,
                     GRID_ROWS = 3;
inline constexpr int POSTER_W = 74, POSTER_H = 111, OVERLAY_H = 18, SHOW_X = 360, SHOW_Y = 51,
                     SHOW_W = 160, SHOW_H = 240, META_X = 363;
inline constexpr int COL_X[2] = {48, 154};
inline constexpr int GRID_VISIBLE = 6;
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
