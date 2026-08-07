#include "app/App.hpp"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    // --- Temporary: video-driver diagnostics ---
    const char *sdl_video = getenv("SDL_VIDEODRIVER");
    printf("[main] SDL_VIDEODRIVER=%s\n", sdl_video ? sdl_video : "(null)");

    int ndrivers = SDL_GetNumVideoDrivers();
    printf("[main] SDL_GetNumVideoDrivers()=%d\n", ndrivers);
    for (int i = 0; i < ndrivers; i++) {
        printf("[main]   driver[%d] = %s\n", i, SDL_GetVideoDriver(i));
    }
    // ------------------------------------------

    miyoofin::App app;
    if (!app.init()) {
        fprintf(stderr, "[main] App initialisation failed\n");
        return 1;
    }

    int result = app.run();
    printf("[main] Exited with code %d\n", result);
    return result;
}