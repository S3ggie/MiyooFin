#ifndef MIYOOFIN_ARTWORK_PRESENTATION_HPP
#define MIYOOFIN_ARTWORK_PRESENTATION_HPP

#include "../data/MediaItem.hpp"
#include <SDL2/SDL.h>

namespace miyoofin {

/// Adapt persisted, platform-neutral artwork data to the SDL presentation
/// type used by the renderers.
inline SDL_Color presentationArtworkColor(const MediaItem &item)
{
    return SDL_Color{item.placeholderArtwork.red,
                     item.placeholderArtwork.green,
                     item.placeholderArtwork.blue, 255};
}

} // namespace miyoofin

#endif // MIYOOFIN_ARTWORK_PRESENTATION_HPP
