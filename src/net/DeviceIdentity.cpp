#include "DeviceIdentity.hpp"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <random>
#include <ctime>

namespace miyoofin {

// -------------------------------------------------------------------
// UUID v4 generation — no external dependencies.
// -------------------------------------------------------------------
std::string DeviceIdentity::generateUuidV4()
{
    std::mt19937_64 rng;

#if defined(__linux__) || defined(__APPLE__)
    std::random_device rd;
    // Seed with 64 bits from the OS entropy source.
    uint64_t seed = ((uint64_t)rd() << 32) ^ rd();
    // Mix in time in case the entropy source is weak.
    seed ^= (uint64_t)std::time(nullptr);
    rng.seed(seed);
#else
    rng.seed(std::time(nullptr));
#endif

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