#ifndef MIYOOFIN_DEVICE_IDENTITY_HPP
#define MIYOOFIN_DEVICE_IDENTITY_HPP

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
    static std::string generateUuidV4();
};

} // namespace miyoofin

#endif // MIYOOFIN_DEVICE_IDENTITY_HPP