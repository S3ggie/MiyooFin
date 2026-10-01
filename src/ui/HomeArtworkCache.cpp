#include "HomeArtworkCache.hpp"

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
    // Compute aspect-fit destination dimensions
    const float imgAspect = static_cast<float>(img.width) / static_cast<float>(img.height);
    const float boxAspect = static_cast<float>(boxW) / static_cast<float>(boxH);
    int dw, dh;
    if (imgAspect > boxAspect) {
        dw = boxW;
        dh = static_cast<int>(boxW / imgAspect + 0.5f);
        if (dh > boxH)
            dh = boxH;
    } else {
        dh = boxH;
        dw = static_cast<int>(boxH * imgAspect + 0.5f);
        if (dw > boxW)
            dw = boxW;
    }
    // Create a temporary surface wrapping the decoded pixels for blitting.
    SDL_Surface* src = SDL_CreateRGBSurfaceFrom(const_cast<unsigned char*>(img.pixels.data()),
                                                img.width, img.height, 32, img.width * 4,
                                                0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (!src)
        return;
    // Always blit into an owned surface.  The source pixels belong to a
    // DecodedImage behind a shared_ptr that store() may replace at any time;
    // wrapping the source directly (the old 1:1 path) created a
    // use-after-free when the old entry was destroyed, producing TV-static
    // garbage and duplicate artwork on neighboring cards.
    SDL_Surface* owned =
        SDL_CreateRGBSurface(0, dw, dh, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (owned) {
        SDL_Rect srcR = {0, 0, img.width, img.height};
        SDL_Rect dstR = {0, 0, dw, dh};
        SDL_BlitScaled(src, &srcR, owned, &dstR);
        cardSurfaces[cacheKey] = owned;
    }
    SDL_FreeSurface(src);
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
