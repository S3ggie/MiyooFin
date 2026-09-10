#include "RemoteExitSignal.hpp"
#include <csignal>

namespace miyoofin {
namespace {
volatile std::sig_atomic_t g_remoteExitRequested = 0;

void handleRemoteExitSignal(int signalNumber) noexcept
{
    if (signalNumber == SIGUSR1)
        g_remoteExitRequested = 1;
}
} // namespace

void installRemoteExitSignalHandler() noexcept
{
    struct sigaction action {};
    action.sa_handler = handleRemoteExitSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    (void)sigaction(SIGUSR1, &action, nullptr);
}

bool consumeRemoteExitRequest() noexcept
{
    if (!g_remoteExitRequested)
        return false;
    g_remoteExitRequested = 0;
    return true;
}
} // namespace miyoofin
