#include "app/App.hpp"
#include "app/RemoteExitSignal.hpp"
#include "diagnostics/PerformanceTelemetry.hpp"
#include "diagnostics/TelemetryConfig.hpp"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    // Unbuffered I/O for debugging
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);
    miyoofin::installRemoteExitSignalHandler();

    // --- Temporary: video-driver diagnostics ---
    const char *sdl_video = getenv("SDL_VIDEODRIVER");
    printf("[main] SDL_VIDEODRIVER=%s\n", sdl_video ? sdl_video : "(null)");

    int ndrivers = SDL_GetNumVideoDrivers();
    printf("[main] SDL_GetNumVideoDrivers()=%d\n", ndrivers);
    for (int i = 0; i < ndrivers; i++) {
        printf("[main]   driver[%d] = %s\n", i, SDL_GetVideoDriver(i));
    }
    // ------------------------------------------

    const miyoofin::TelemetryConfig telemetryConfig =
        miyoofin::TelemetryConfig::fromEnvironment();
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
