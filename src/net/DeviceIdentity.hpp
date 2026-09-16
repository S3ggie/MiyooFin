#ifndef MIYOOFIN_DEVICE_IDENTITY_HPP
#define MIYOOFIN_DEVICE_IDENTITY_HPP

#include <cstdint>
#include <string>

namespace miyoofin {

/// Manages a persistent device identifier (UUID v4).
/// On first launch, generates a UUID and saves it to "device.txt".
/// On subsequent launches, loads the saved UUID.
struct DeviceIdentity {
    /// Load or create the persistent device ID.
    /// @param filePath  Path to the device ID file (default "device.txt").
    static std::string loadOrCreate(const std::string &filePath = "device.txt");

    /// Generate a UUID v4 string (without external libraries).
    /// Never throws due to unavailable OS entropy: when std::random_device
    /// cannot be used (e.g. unreadable /dev/urandom), falls back to
    /// fallbackSeed(). The success-path format is unchanged.
    static std::string generateUuidV4();

    /// Build a UUID v4 from an explicit 64-bit seed.
    /// Same byte layout/version/variant/format as generateUuidV4().
    static std::string uuidFromSeed(uint64_t seed);

    /// Non-throwing fallback seed for environments where std::random_device
    /// is unavailable. Mixes steady/system clocks, pid, and address material.
    /// Never throws and never touches /dev/urandom.
    static uint64_t fallbackSeed() noexcept;
};

} // namespace miyoofin

#endif // MIYOOFIN_DEVICE_IDENTITY_HPP