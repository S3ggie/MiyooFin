#ifndef MIYOOFIN_ARTWORK_PRESENTATION_HPP
#define MIYOOFIN_ARTWORK_PRESENTATION_HPP

#include "../data/MediaItem.hpp"
#include <SDL2/SDL.h>

namespace miyoofin {

/// Derive the deterministic fallback colour used by the renderers from the
/// title.  This is presentation state and is intentionally not persisted.
inline SDL_Color presentationArtworkColor(const MediaItem& item)
{
    const std::size_t length = item.title.size();
    const std::size_t r0 = (length * 37 + 80) & 0xff;
    const std::size_t g0 = (length * 53 + 160) & 0xff;
    const std::size_t b0 = (length * 71 + 240) & 0xff;
    return SDL_Color{static_cast<Uint8>(80 + r0 % 120), static_cast<Uint8>(80 + g0 % 120),
                     static_cast<Uint8>(80 + b0 % 120), 255};
}

} // namespace miyoofin

#endif // MIYOOFIN_ARTWORK_PRESENTATION_HPP
