#include "ArtworkUrl.hpp"
#include <cstdio>

namespace miyoofin {

const char *imageTypeToString(ImageType type)
{
    switch (type) {
        case ImageType::Primary: return "Primary";
        case ImageType::Thumb:   return "Thumb";
    }
    return "Primary";
}

std::string buildImageUrl(const std::string &baseUrl,
                          const std::string &itemId,
                          ImageType type,
                          const std::string &imageTag,
                          int maxWidth,
                          int maxHeight)
{
    // Strip trailing slash from baseUrl
    std::string base = baseUrl;
    while (!base.empty() && base.back() == '/')
        base.pop_back();

    char buf[1024];
    std::snprintf(buf, sizeof(buf),
        "%s/Items/%s/Images/%s?"
        "format=jpg&maxWidth=%d&maxHeight=%d&quality=80&tag=%s",
        base.c_str(),
        itemId.c_str(),
        imageTypeToString(type),
        maxWidth,
        maxHeight,
        imageTag.c_str());

    return std::string(buf);
}

} // namespace miyoofin
