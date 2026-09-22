#ifndef MIYOOFIN_TEST_SUPPORT_HPP
#define MIYOOFIN_TEST_SUPPORT_HPP

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>
#include <curl/curl.h>

#include "miyoofin/version.hpp"
#include "../src/net/JellyfinApi.hpp"
#include "../src/net/ArtworkUrl.hpp"
#include "../src/net/Session.hpp"
#include "../src/net/DeviceIdentity.hpp"
#include "../src/data/MediaItem.hpp"
#include "../src/ui/BitmapFont.hpp"
#include "../src/ui/screens/HomeScreen.hpp"
#include "../src/ui/screens/ConnectScreen.hpp"
#include "../src/ui/screens/ServerEntryScreen.hpp"
#include "../src/ui/screens/LoginScreen.hpp"
#include "../src/ui/OnScreenKeyboard.hpp"
#include "../src/image/ImageDecoder.hpp"
#include "../src/cache/ImageCache.hpp"
#include "../src/cache/LibraryCache.hpp"
#include "../src/cache/SyncState.hpp"
#include "../src/cache/OfflineCatalog.hpp"
#include "../src/playback/OfflineLibraryProjection.hpp"
#include "../src/library/OfflineLibraryQuery.hpp"
#include "../src/net/HttpClient.hpp"
#include "../src/net/RouteRequest.hpp"
#include "../src/net/RouteStatus.hpp"
#include "../src/net/ServerAddress.hpp"
#include "../src/net/ClockCheck.hpp"
#include "../src/ui/ArtworkLayout.hpp"
#include "../src/ui/ArtworkPresentation.hpp"
#include "../src/data/MovieTitle.hpp"
#include "../src/ui/ShowsBrowser.hpp"
#include "../src/ui/screens/EpisodeBrowserScreen.hpp"
#include "../src/ui/screens/SeriesScreen.hpp"
#include "../src/ui/screens/MovieDetailsScreen.hpp"
#include "../src/app/ScreenStack.hpp"
#include "../src/app/RemoteExitSignal.hpp"
#include "../src/app/DisplaySizing.hpp"
#include "../src/catalog/CatalogDb.hpp"
#include "../src/diagnostics/UiDiagnostics.hpp"
#include "../src/playback/PlaybackRequest.hpp"
#include "../src/playback/OfflinePlaybackJournal.hpp"
#include "../src/download/DownloadTypes.hpp"
#include "../src/download/DownloadManager.hpp"
#include "../src/download/DownloadSupport.hpp"
#include "../src/download/DownloadReconcile.hpp"
#include "../src/download/DownloadUi.hpp"
#include "../src/input/InputManager.hpp"

using namespace miyoofin;

namespace miyoofin_test {

inline int failures = 0;

inline std::string readTestBytes(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Whitespace-insensitive production-source matcher for structural tests.
// Reformatting (reindent, line-split, spacing) must not break a structural
// assertion, so every matcher below compares with ALL whitespace stripped
// from both haystack and needle. Single-token needles behave exactly like
// std::string::find; multi-token needles additionally survive reflow.
// NOTE: stripped positions from sourcePos/sourceRPos live in token space —
// use them only for ordering comparisons, never as offsets into the raw
// string. Region slicing must anchor on whitespace-free tokens (function
// signatures, comments, identifiers), which reformatting cannot alter.
inline std::string sourceTokenString(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c)))
            out.push_back(c);
    return out;
}

// Comment-stripper for structural source scans: replaces `//...` and
// `/*...*/` with spaces (newlines preserved, so raw offsets stay stable)
// while leaving string/char literals intact (a URL such as "http://..."
// must not start a line comment). Structural checks must run on the result:
// comment prose mentioning an identifier can never satisfy them.
inline std::string stripSourceComments(const std::string& text)
{
    std::string out = text;
    enum State
    {
        Code,
        Line,
        Block,
        Str,
        Chr
    };
    State state = Code;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        const char next = (i + 1 < out.size()) ? out[i + 1] : '\0';
        switch (state) {
        case Code:
            if (c == '/' && next == '/') {
                out[i] = out[i + 1] = ' ';
                ++i;
                state = Line;
            } else if (c == '/' && next == '*') {
                out[i] = out[i + 1] = ' ';
                ++i;
                state = Block;
            } else if (c == '"') {
                state = Str;
            } else if (c == '\'') {
                state = Chr;
            }
            break;
        case Line:
            if (c == '\n')
                state = Code;
            else
                out[i] = ' ';
            break;
        case Block:
            if (c == '*' && next == '/') {
                out[i] = out[i + 1] = ' ';
                ++i;
                state = Code;
            } else if (c != '\n') {
                out[i] = ' ';
            }
            break;
        case Str:
            if (c == '\\' && i + 1 < out.size())
                ++i;
            else if (c == '"')
                state = Code;
            break;
        case Chr:
            if (c == '\\' && i + 1 < out.size())
                ++i;
            else if (c == '\'')
                state = Code;
            break;
        }
    }
    return out;
}

inline bool sourceContains(const std::string& haystack, const std::string& needle)
{
    if (haystack.empty() || needle.empty())
        return false;
    const std::string token = sourceTokenString(needle);
    if (token.empty())
        return false;
    return sourceTokenString(haystack).find(token) != std::string::npos;
}

inline bool sourceLacks(const std::string& haystack, const std::string& needle)
{
    // Fail-safe: an empty/unreadable source must never satisfy an absence
    // check (otherwise the check passes vacuously when the file is missing).
    if (haystack.empty())
        return false;
    if (needle.empty())
        return true;
    return !sourceContains(haystack, needle);
}

inline std::size_t sourceCount(const std::string& haystack, const std::string& needle)
{
    if (haystack.empty() || needle.empty())
        return 0;
    const std::string hay = sourceTokenString(haystack);
    const std::string token = sourceTokenString(needle);
    if (token.empty())
        return 0;
    std::size_t count = 0, pos = 0;
    while ((pos = hay.find(token, pos)) != std::string::npos) {
        ++count;
        pos += token.size();
    }
    return count;
}

inline std::size_t sourcePos(const std::string& haystack, const std::string& needle)
{
    if (haystack.empty() || needle.empty())
        return std::string::npos;
    const std::string token = sourceTokenString(needle);
    if (token.empty())
        return std::string::npos;
    return sourceTokenString(haystack).find(token);
}

inline std::size_t sourceRPos(const std::string& haystack, const std::string& needle)
{
    if (haystack.empty() || needle.empty())
        return std::string::npos;
    const std::string token = sourceTokenString(needle);
    if (token.empty())
        return std::string::npos;
    return sourceTokenString(haystack).rfind(token);
}

// Raw slice of the function starting at `signature` (a whitespace-free
// anchor such as "void HomeScreen::finishFetch("), ending at the next
// top-level function/namespace boundary. Lets a structural check pin one
// function instead of searching the whole translation unit.
inline std::string sourceFunction(const std::string& haystack, const std::string& signature)
{
    const std::size_t begin = haystack.find(signature);
    if (begin == std::string::npos)
        return {};
    static const char* const kEndMarkers[] = {
        "\nvoid ",       "\nbool ",          "\nint ",
        "\nstatic ",     "\nHomeScreen::",   "\nDownloadManager::",
        "\nCatalogDb::", "\n} // namespace",
    };
    std::size_t end = std::string::npos;
    for (const char* marker : kEndMarkers) {
        const std::size_t pos = haystack.find(marker, begin + 1);
        if (pos != std::string::npos && (end == std::string::npos || pos < end))
            end = pos;
    }
    if (end == std::string::npos)
        end = haystack.size();
    return haystack.substr(begin, end - begin);
}

// Token-space slice between the first `startNeedle` and the first
// `endNeedle` after it (empty when either anchor is missing). For pinning
// ordered regions (handler bodies, publish windows) without fixed char
// windows that reformatting would shift.
inline std::string sourceBetween(const std::string& haystack, const std::string& startNeedle,
                                 const std::string& endNeedle)
{
    const std::string hay = sourceTokenString(haystack);
    const std::string start = sourceTokenString(startNeedle);
    const std::string end = sourceTokenString(endNeedle);
    if (hay.empty() || start.empty() || end.empty())
        return {};
    const std::size_t begin = hay.find(start);
    if (begin == std::string::npos)
        return {};
    const std::size_t stop = hay.find(end, begin + start.size());
    if (stop == std::string::npos)
        return {};
    return hay.substr(begin + start.size(), stop - begin - start.size());
}

inline int finish(const char* group)
{
    if (failures == 0) {
        std::printf("[%s] passed\n", group);
        return 0;
    }
    std::printf("[%s] %d test(s) FAILED.\n", group, failures);
    return 1;
}

} // namespace miyoofin_test

using miyoofin_test::readTestBytes;
using miyoofin_test::sourceBetween;
using miyoofin_test::sourceContains;
using miyoofin_test::sourceCount;
using miyoofin_test::sourceFunction;
using miyoofin_test::sourceLacks;
using miyoofin_test::sourcePos;
using miyoofin_test::sourceRPos;
using miyoofin_test::sourceTokenString;
using miyoofin_test::stripSourceComments;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                          \
            ++miyoofin_test::failures;                                                             \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        if ((a) != (b)) {                                                                          \
            std::printf("  FAIL %s:%d: expected \"%s\", got \"%s\"\n", __FILE__, __LINE__,         \
                        std::string(b).c_str(), std::string(a).c_str());                           \
            ++miyoofin_test::failures;                                                             \
        }                                                                                          \
    } while (0)

#endif // MIYOOFIN_TEST_SUPPORT_HPP
