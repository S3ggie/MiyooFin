#include "app/App.hpp"
#include "app/RemoteExitSignal.hpp"
#include "update/AppDir.hpp"
#include "diagnostics/PerformanceTelemetry.hpp"
#include "diagnostics/TelemetryConfig.hpp"
#include "../vendor/sqlite/sqlite3.h"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    // Unbuffered I/O for debugging
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);
    miyoofin::installRemoteExitSignalHandler();

    // --- Temporary: video-driver diagnostics ---
    const char* sdl_video = getenv("SDL_VIDEODRIVER");
    printf("[main] SDL_VIDEODRIVER=%s\n", sdl_video ? sdl_video : "(null)");

    int ndrivers = SDL_GetNumVideoDrivers();
    printf("[main] SDL_GetNumVideoDrivers()=%d\n", ndrivers);
    for (int i = 0; i < ndrivers; i++) {
        printf("[main]   driver[%d] = %s\n", i, SDL_GetVideoDriver(i));
    }
    // ------------------------------------------

    // Redirect SQLite temp files to the SD card.  The Miyoo Mini Plus has a
    // 49 MB /tmp tmpfs that can fill up and cause SQLITE_FULL on large catalog
    // transactions.  The directory lives under the app's cache tree so it is
    // never fed to the image-cache janitor.
    {
        std::string baseDir;
        if (miyoofin::appDir(baseDir)) {
            const std::string tempDir = miyoofin::sqliteTempDirPath(baseDir);
            // mkdir -p (ignore EEXIST)
            for (std::size_t i = 1; i <= tempDir.size(); ++i) {
                if (i == tempDir.size() || tempDir[i] == '/') {
                    const std::string component = tempDir.substr(0, i);
                    if (!component.empty()) {
                        ::mkdir(component.c_str(), 0755);
                        // EEXIST is fine; other errors fall back to default.
                    }
                }
            }
            struct stat st;
            if (::stat(tempDir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                sqlite3_temp_directory = sqlite3_mprintf("%s", tempDir.c_str());
                printf("[main] SQLite temp dir: %s\n", tempDir.c_str());
            } else {
                printf("[main] SQLite temp dir fallback (mkdir failed): %s\n", tempDir.c_str());
            }
        }
    }

    const miyoofin::TelemetryConfig telemetryConfig = miyoofin::TelemetryConfig::fromEnvironment();
    miyoofin::performanceTelemetry().start(telemetryConfig);

    int result = 0;
    {
        miyoofin::App app;
        if (!app.init()) {
            fprintf(stderr, "[main] App initialisation failed\n");
            result = 1;
        } else {
            result = app.run();
        }
    }

    miyoofin::performanceTelemetry().stop();
    printf("[main] Exited with code %d\n", result);
    return result;
}
