#ifndef MIYOOFIN_ARTWORK_URL_HPP
#define MIYOOFIN_ARTWORK_URL_HPP

#include <string>

namespace miyoofin {

/// Image types supported by the Jellyfin artwork API.
enum class ImageType {
    Primary,
    Thumb
};

/// Convert an ImageType to its Jellyfin URL path segment.
const char *imageTypeToString(ImageType type);

/// Build an authenticated Jellyfin image URL.
///
/// Produces URLs of the form:
///   {baseUrl}/Items/{itemId}/Images/{imageType}?format=jpg&maxWidth=W&maxHeight=H&quality=80&tag=T
///
/// Access tokens are NOT included in the URL — callers pass them as
/// HTTP headers instead.
std::string buildImageUrl(const std::string &baseUrl,
                          const std::string &itemId,
                          ImageType type,
                          const std::string &imageTag,
                          int maxWidth,
                          int maxHeight);

} // namespace miyoofin

#endif // MIYOOFIN_ARTWORK_URL_HPP
