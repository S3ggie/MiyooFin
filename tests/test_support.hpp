#ifndef MIYOOFIN_TEST_SUPPORT_HPP
#define MIYOOFIN_TEST_SUPPORT_HPP

#include <atomic>
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

inline std::string readTestBytes(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

inline int finish(const char *group)
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

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++miyoofin_test::failures; \
        } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            std::printf("  FAIL %s:%d: expected \"%s\", got \"%s\"\n", \
                        __FILE__, __LINE__, std::string(b).c_str(), \
                        std::string(a).c_str()); \
            ++miyoofin_test::failures; \
        } \
    } while (0)

#endif // MIYOOFIN_TEST_SUPPORT_HPP
