#ifndef MIYOOFIN_DISPLAY_SIZING_HPP
#define MIYOOFIN_DISPLAY_SIZING_HPP

#include "miyoofin/version.hpp"

namespace miyoofin {

struct DisplayDimensions {
    int width;
    int height;
};

constexpr DisplayDimensions displayDimensionsFor(int reportedWidth,
                                                 int reportedHeight) noexcept
{
    if (reportedWidth > 0 && reportedHeight > 0)
        return {reportedWidth, reportedHeight};
    return {SCREEN_W, SCREEN_H};
}

} // namespace miyoofin

#endif // MIYOOFIN_DISPLAY_SIZING_HPP
