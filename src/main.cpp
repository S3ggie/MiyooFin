#include "app/App.hpp"
#include <cstdio>
#include <cstdlib>

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    miyoofin::App app;
    if (!app.init()) {
        fprintf(stderr, "[main] App initialisation failed\n");
        return 1;
    }

    int result = app.run();
    printf("[main] Exited with code %d\n", result);
    return result;
}