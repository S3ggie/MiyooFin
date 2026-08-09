#include "PlaybackRequest.hpp"
#include <cstdio>
#include <cstring>

namespace miyoofin {

static const char *DEFAULT_PATH = "playback-request.txt";

const char *PlaybackRequest::defaultPath()
{
    return DEFAULT_PATH;
}

// -------------------------------------------------------------------
// write
// -------------------------------------------------------------------
bool PlaybackRequest::write(const std::string &itemId,
                            const std::string &itemType,
                            long long resumeTicks,
                            std::string &error)
{
    return writeTo(DEFAULT_PATH, itemId, itemType, resumeTicks, error);
}

bool PlaybackRequest::writeTo(const std::string &path,
                              const std::string &itemId,
                              const std::string &itemType,
                              long long resumeTicks,
                              std::string &error)
{
    if (itemId.empty()) {
        error = "item_id is empty";
        return false;
    }
    if (itemType.empty()) {
        error = "item_type is empty";
        return false;
    }

    FILE *f = std::fopen(path.c_str(), "w");
    if (!f) {
        error = "failed to open ";
        error += path;
        return false;
    }

    fprintf(f, "item_id=%s\n", itemId.c_str());
    fprintf(f, "item_type=%s\n", itemType.c_str());
    fprintf(f, "resume_ticks=%lld\n", resumeTicks < 0 ? 0LL : resumeTicks);

    std::fclose(f);
    return true;
}

// -------------------------------------------------------------------
// exists
// -------------------------------------------------------------------
bool PlaybackRequest::exists()
{
    return existsAt(DEFAULT_PATH);
}

bool PlaybackRequest::existsAt(const std::string &path)
{
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// -------------------------------------------------------------------
// remove
// -------------------------------------------------------------------
bool PlaybackRequest::remove()
{
    return removeAt(DEFAULT_PATH);
}

bool PlaybackRequest::removeAt(const std::string &path)
{
    // remove() returns 0 on success or if file does not exist
    return std::remove(path.c_str()) == 0 || !existsAt(path);
}

} // namespace miyoofin
