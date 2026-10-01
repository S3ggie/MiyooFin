#ifndef MIYOOFIN_HOME_ARTWORK_CACHE_HPP
#define MIYOOFIN_HOME_ARTWORK_CACHE_HPP

#include "ArtworkLayout.hpp"
#include <SDL2/SDL.h>
#include <deque>
#include <map>
#include <set>
#include <string>

namespace miyoofin {

// UI-thread cache for Home's row artwork: decoded images keyed by artwork
// identity (bounded LRU) and pre-scaled card surfaces (bounded, flushed when
// over the cap). Owns the SDL surfaces it creates and frees them on
// destruction, so HomeScreen no longer tracks them by hand.
class HomeArtworkCache
{
  public:
    // Upper bound on cached card surfaces. Each row artwork entry can produce
    // surfaces at several box sizes; the cap prevents unbounded growth when
    // distinct keys accumulate faster than row eviction cleans them up.
    static constexpr int kMaxCardSurfaces = 128;

    HomeArtworkCache() = default;
    HomeArtworkCache(const HomeArtworkCache&) = delete;
    HomeArtworkCache& operator=(const HomeArtworkCache&) = delete;
    ~HomeArtworkCache()
    {
        clearCardSurfaces();
    }

    /// Row artwork state map; public so tests can inspect it.
    std::map<std::string, RowArtworkEntry> entries;

    /// LRU order: oldest key at the front. Loaded keys occur once.
    std::deque<std::string> order;

    /// Pre-scaled card surfaces (avoids per-frame create/scale/free).
    std::map<std::string, SDL_Surface*> cardSurfaces;

    // Marks `key` most recently used.
    void touch(const std::string& key);

    // Stores a decoded image for `key`, drops card surfaces made from the
    // previous image, and evicts down to the RAM limit without evicting any
    // key in `protectedKeys` (images currently being rendered).
    void store(const std::string& key, DecodedImage image,
               const std::set<std::string>& protectedKeys);

    // Evicts least-recently-used unprotected keys (and their card surfaces)
    // while over ROW_ARTWORK_RAM_LIMIT. A temporary overflow is preferred to
    // evicting a protected image.
    void evictIfNeeded(const std::set<std::string>& protectedKeys);

    // Builds an owned, aspect-fit card surface for `cacheKey` from `img`.
    void prepareCardSurface(const std::string& cacheKey, const DecodedImage& img, int boxW,
                            int boxH);

    void clearCardSurfaces();

  private:
    // Frees card surfaces whose key is `key` or starts with "key:".
    void dropCardSurfacesFor(const std::string& key);
};

} // namespace miyoofin

#endif // MIYOOFIN_HOME_ARTWORK_CACHE_HPP
