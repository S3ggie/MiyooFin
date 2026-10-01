#include "HomeArtworkCache.hpp"

#include "UiKit.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace miyoofin {

void HomeArtworkCache::dropCardSurfacesFor(const std::string& key)
{
    for (auto it = cardSurfaces.begin(); it != cardSurfaces.end();) {
        if (it->first.compare(0, key.size(), key) == 0 &&
            (it->first.size() == key.size() || it->first[key.size()] == ':')) {
            if (it->second)
                SDL_FreeSurface(it->second);
            it = cardSurfaces.erase(it);
        } else {
            ++it;
        }
    }
}

void HomeArtworkCache::touch(const std::string& key)
{
    auto it = std::find(order.begin(), order.end(), key);
    if (it != order.end())
        order.erase(it);
    order.push_back(key);
}

void HomeArtworkCache::evictIfNeeded(const std::set<std::string>& protectedKeys)
{
    while (static_cast<int>(order.size()) > ROW_ARTWORK_RAM_LIMIT) {
        auto victim = std::find_if(order.begin(), order.end(), [&](const std::string& key) {
            return protectedKeys.find(key) == protectedKeys.end();
        });
        // A temporary overflow is preferable to evicting an image being
        // rendered.  This is only possible when every cached key is visible.
        if (victim == order.end())
            break;
        dropCardSurfacesFor(*victim);
        entries.erase(*victim);
        order.erase(victim);
    }
}

void HomeArtworkCache::store(const std::string& key, DecodedImage image,
                             const std::set<std::string>& protectedKeys)
{
    // Surfaces created by prepareCardSurface own their pixels, but evicting
    // them here ensures stale artwork is never served after an artwork
    // refresh for the same item.
    dropCardSurfacesFor(key);
    RowArtworkEntry& entry = entries[key];
    entry.status = RowArtworkStatus::Loaded;
    entry.image = std::make_shared<DecodedImage>(std::move(image));
    touch(key);
    evictIfNeeded(protectedKeys);
}

void HomeArtworkCache::prepareCardSurface(const std::string& cacheKey, const DecodedImage& img,
                                          int boxW, int boxH)
{
    if (img.empty() || boxW <= 0 || boxH <= 0)
        return;
    // Prevent unbounded card-surface cache growth.  When the cache exceeds
    // the limit, flush it entirely — it will be rebuilt on the next frame
    // for whatever is currently visible.
    if (static_cast<int>(cardSurfaces.size()) > kMaxCardSurfaces)
        clearCardSurfaces();
    // Free old cached surface for this key
    auto it = cardSurfaces.find(cacheKey);
    if (it != cardSurfaces.end()) {
        if (it->second)
            SDL_FreeSurface(it->second);
        cardSurfaces.erase(it);
    }
    // Owned, exactly boxW x boxH, never letterboxed (cover or ambient fill).
    // It copies the pixels, so the shared DecodedImage may be replaced freely.
    if (SDL_Surface* owned = ui::artworkSurface(img, boxW, boxH))
        cardSurfaces[cacheKey] = owned;
}

void HomeArtworkCache::clearCardSurfaces()
{
    for (auto& kv : cardSurfaces) {
        if (kv.second)
            SDL_FreeSurface(kv.second);
    }
    cardSurfaces.clear();
}

} // namespace miyoofin
