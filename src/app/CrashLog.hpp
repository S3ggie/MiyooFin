#ifndef MIYOOFIN_CRASH_LOG_HPP
#define MIYOOFIN_CRASH_LOG_HPP

namespace miyoofin {

/// Installs fatal-signal handlers that append a short backtrace to `crash.log` in the
/// working directory, then die the way the signal would have. On the device the app has
/// no terminal, so this is the only trace of a crash. Safe to call once at startup.
void installCrashLog();

} // namespace miyoofin

#endif
