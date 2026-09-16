#include "DeviceIdentity.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <exception>
#include <random>
#include <ctime>

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace miyoofin {

// -------------------------------------------------------------------
// UUID v4 generation — no external dependencies.
// -------------------------------------------------------------------
std::string DeviceIdentity::uuidFromSeed(uint64_t seed)
{
    std::mt19937_64 rng(seed);

    uint8_t bytes[16];
    for (int i = 0; i < 16; ++i) {
        bytes[i] = (uint8_t)(rng() & 0xFF);
    }

    // Set version (4) and variant (RFC 4122)
    bytes[6] = (uint8_t)((bytes[6] & 0x0F) | 0x40);
    bytes[8] = (uint8_t)((bytes[8] & 0x3F) | 0x80);

    char buf[40];
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);
    return std::string(buf);
}

// Non-throwing fallback seed: portable, no /dev/urandom dependency.
// Goal is a stable-shaped unique device id, not cryptographic randomness.
uint64_t DeviceIdentity::fallbackSeed() noexcept
{
    static uint64_t callCount = 0;
    ++callCount;

    uint64_t seed = callCount * 0x9E3779B97F4A7C15ULL;

    const uint64_t steady =
        (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    const uint64_t system =
        (uint64_t)std::chrono::system_clock::now().time_since_epoch().count();
    seed ^= steady + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2);
    seed ^= system + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2);

    // Stack/heap address material (values only, never logged).
    int stackMarker = 0;
    static int anchor = 0;
    const uint64_t addrMix = (uint64_t)(uintptr_t)&stackMarker
        ^ ((uint64_t)(uintptr_t)&anchor << 33);
    seed ^= addrMix + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2);

#if defined(__linux__) || defined(__APPLE__)
    seed ^= (uint64_t)(uintptr_t)::getpid() * 0x9E3779B97F4A7C15ULL;
#endif

    // splitmix64-style avalanche so clustered inputs spread across 64 bits.
    seed += 0x9E3779B97F4A7C15ULL;
    seed = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9ULL;
    seed = (seed ^ (seed >> 27)) * 0x94D049BB133111EBULL;
    seed ^= (seed >> 31);
    return seed;
}

std::string DeviceIdentity::generateUuidV4()
{
#if defined(__linux__) || defined(__APPLE__)
    try {
        std::random_device rd;
        // Seed with 64 bits from the OS entropy source.
        uint64_t seed = ((uint64_t)rd() << 32) ^ rd();
        // Mix in time in case the entropy source is weak.
        seed ^= (uint64_t)std::time(nullptr);
        return uuidFromSeed(seed);
    } catch (const std::exception &) {
        // e.g. libstdc++ throws std::runtime_error when /dev/urandom
        // cannot be opened (non-root OnionOS ships it 0660 root:root).
        return uuidFromSeed(fallbackSeed());
    } catch (...) {
        return uuidFromSeed(fallbackSeed());
    }
#else
    return uuidFromSeed((uint64_t)std::time(nullptr));
#endif
}

// -------------------------------------------------------------------
// Load or create the persistent device ID.
// -------------------------------------------------------------------
std::string DeviceIdentity::loadOrCreate(const std::string &filePath)
{
    // Try to load an existing ID
    FILE *f = std::fopen(filePath.c_str(), "r");
    if (f) {
        char buf[64];
        if (std::fgets(buf, sizeof(buf), f)) {
            size_t len = std::strlen(buf);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' '))
                buf[--len] = '\0';
            std::fclose(f);
            if (len > 0) {
                return std::string(buf);
            }
        }
        std::fclose(f);
    }

    // Generate a new one and persist it
    std::string id = generateUuidV4();
    f = std::fopen(filePath.c_str(), "w");
    if (f) {
        std::fprintf(f, "%s\n", id.c_str());
        std::fclose(f);
    }
    return id;
}

} // namespace miyoofin