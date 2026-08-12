#pragma once

#include <ctime>

namespace miyoofin {

/// Epoch-seconds for 2020-01-01 00:00:00 UTC.
/// The Miyoo Mini Plus shipped in 2021.  Any valid TLS certificate
/// in use would have been issued no earlier than ~2020.
static constexpr std::time_t kMinReasonableEpoch = 1577836800L;

/// Returns true when the given epoch is obviously too old to be a
/// valid system clock — a one-sided check that catches RTC resets
/// to the default 1970-01 date.
inline bool isEpochObviouslyInvalid(std::time_t epoch)
{
    return epoch < kMinReasonableEpoch;
}

/// Pure, deterministic classifier: should we show the clock-error
/// hint?  True only when peer verification failed AND the system
/// clock is below the minimum reasonable epoch.
inline bool shouldShowClockError(bool peerVerificationFailed, std::time_t epoch)
{
    return peerVerificationFailed && isEpochObviouslyInvalid(epoch);
}

/// Friendly one-line message shown when an HTTPS certificate
/// verification failure is likely caused by an incorrect system clock.
inline const char *kClockErrorMessage =
    "Clock wrong. OnionOS: Apps > Tweaks > System > Date/time > auto time.";

} // namespace miyoofin
