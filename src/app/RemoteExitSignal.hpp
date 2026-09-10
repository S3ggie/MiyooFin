#ifndef MIYOOFIN_REMOTE_EXIT_SIGNAL_HPP
#define MIYOOFIN_REMOTE_EXIT_SIGNAL_HPP

namespace miyoofin {
void installRemoteExitSignalHandler() noexcept;
bool consumeRemoteExitRequest() noexcept;
} // namespace miyoofin

#endif
