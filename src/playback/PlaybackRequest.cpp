#include "PlaybackRequest.hpp"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <cctype>

namespace miyoofin {

static const char *DEFAULT_PATH = "playback-request.txt";
static const char *DEFAULT_RESULT_PATH = "playback-result.txt";

static std::string readValue(const std::string &content, const char *key)
{
    const std::string prefix = std::string(key) + "=";
    size_t start = 0;
    while (start <= content.size()) {
        size_t end = content.find('\n', start);
        if (end == std::string::npos) end = content.size();
        size_t lineEnd = end;
        if (lineEnd > start && content[lineEnd - 1] == '\r') --lineEnd;
        if (content.compare(start, prefix.size(), prefix) == 0)
            return content.substr(start + prefix.size(),
                                  lineEnd - start - prefix.size());
        if (end == content.size()) break;
        start = end + 1;
    }
    return {};
}

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
    return writeWithSourceTo(path, itemId, itemType, resumeTicks, "jellyfin", "", error);
}

static bool safePlaybackField(const std::string &value)
{
    if (value.empty() || value.size() > 255) return false;
    for (unsigned char c : value)
        if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.')) return false;
    return true;
}

bool PlaybackRequest::writeWithSourceTo(const std::string &path,
                                        const std::string &itemId,
                                        const std::string &itemType,
                                        long long resumeTicks,
                                        const std::string &sourceMode,
                                        const std::string &scope,
                                        std::string &error)
{
    if (!safePlaybackField(itemId)) {
        error = "item_id is unsafe";
        return false;
    }
    if (itemType.empty()) {
        error = "item_type is empty";
        return false;
    }
    if (sourceMode != "jellyfin" && sourceMode != "local") { error="invalid source_mode"; return false; }
    if (sourceMode == "local" && !safePlaybackField(scope)) { error="download_scope is unsafe"; return false; }

    FILE *f = std::fopen(path.c_str(), "w");
    if (!f) {
        error = "failed to open ";
        error += path;
        return false;
    }

    fprintf(f, "item_id=%s\n", itemId.c_str());
    fprintf(f, "item_type=%s\n", itemType.c_str());
    fprintf(f, "resume_ticks=%lld\n", resumeTicks < 0 ? 0LL : resumeTicks);
    fprintf(f, "source_mode=%s\n", sourceMode.c_str());
    if (sourceMode == "local") fprintf(f, "download_scope=%s\n", scope.c_str());

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

bool PlaybackRequest::consumeResult(const std::string &expectedItemId,
                                    std::int64_t &positionTicks,
                                    std::string &error)
{
    return consumeResultFrom(DEFAULT_RESULT_PATH, expectedItemId,
                             positionTicks, error);
}

bool PlaybackRequest::consumeResultFrom(const std::string &path,
                                        const std::string &expectedItemId,
                                        std::int64_t &positionTicks,
                                        std::string &error)
{
    error.clear();
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) {
        error = "playback result not found";
        removeAt(path);
        return false;
    }

    std::string content;
    char buf[256];
    size_t count = 0;
    while ((count = std::fread(buf, 1, sizeof(buf), f)) > 0)
        content.append(buf, count);
    const bool readOk = !std::ferror(f);
    std::fclose(f);
    removeAt(path);
    if (!readOk) {
        error = "failed to read playback result";
        return false;
    }

    const std::string itemId = readValue(content, "item_id");
    const std::string ticksText = readValue(content, "position_ticks");
    if (itemId.empty()) {
        error = "missing item_id";
        return false;
    }
    if (itemId != expectedItemId) {
        error = "item_id mismatch";
        return false;
    }
    if (ticksText.empty()) {
        error = "missing position_ticks";
        return false;
    }

    errno = 0;
    char *end = nullptr;
    const long long parsed = std::strtoll(ticksText.c_str(), &end, 10);
    if (errno == ERANGE || end == ticksText.c_str() || *end != '\0' ||
        parsed < 0 ||
        parsed > std::numeric_limits<std::int64_t>::max()) {
        error = "invalid position_ticks";
        return false;
    }
    positionTicks = static_cast<std::int64_t>(parsed);
    return true;
}

bool PlaybackRequest::parseResult(const std::string &content, PlaybackResult &r, std::string &error){error.clear();r={};r.itemId=readValue(content,"item_id");std::string ticks=readValue(content,"position_ticks");if(r.itemId.empty()||ticks.empty()){error="missing playback result fields";return false;}char*end=nullptr;errno=0;long long n=strtoll(ticks.c_str(),&end,10);if(errno||end==ticks.c_str()||*end||n<0){error="invalid position_ticks";return false;}r.positionTicks=n;r.itemType=readValue(content,"item_type");r.sourceMode=readValue(content,"source_mode");std::string base=readValue(content,"base_resume_ticks");if(!base.empty())r.baseResumeTicks=strtoll(base.c_str(),nullptr,10);std::string reported=readValue(content,"server_reported");if(!reported.empty())r.serverReported=reported=="1";return true;}
bool PlaybackRequest::readResultFrom(const std::string&p,PlaybackResult&r,std::string&e){FILE*f=fopen(p.c_str(),"r");if(!f){e="playback result not found";return false;}std::string b;char q[256];size_t n;while((n=fread(q,1,sizeof q,f)))b.append(q,n);bool ok=!ferror(f);fclose(f);return ok&&parseResult(b,r,e);}

bool PlaybackRequest::advanceResultConsumption(bool &pending,
                                               int &delayUpdates)
{
    if (!pending) return false;
    if (delayUpdates > 0) {
        --delayUpdates;
        return false;
    }
    pending = false;
    return true;
}

} // namespace miyoofin
