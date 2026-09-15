#ifndef MIYOOFIN_UPDATE_SHA256_HPP
#define MIYOOFIN_UPDATE_SHA256_HPP

#include <atomic>
#include <cstdint>
#include <string>

namespace miyoofin {

/// Self-contained SHA-256 implementation (no external dependencies).
class Sha256 {
public:
    Sha256();
    void update(const void *data, std::size_t len);
    void final(unsigned char out[32]);
    /// Hex-encode 32 raw bytes into a 64-char lowercase string.
    static std::string hex(const unsigned char raw[32]);

private:
    void processBlock(const unsigned char block[64]);
    std::uint32_t m_state[8];
    std::uint64_t m_bitLen;
    unsigned char m_buf[64];
    std::size_t m_bufLen;
};

/// Compute SHA-256 of a file, writing the 64-char lowercase hex digest.
/// Reads in 64 KiB chunks.  If cancellation is non-null and becomes true
/// during the read, the function returns false without setting hex.
/// Returns false on file-open or read error.
bool sha256File(const std::string &path, std::string &hexOut,
                std::string &errorOut,
                const std::atomic<bool> *cancellation = nullptr);

} // namespace miyoofin

#endif // MIYOOFIN_UPDATE_SHA256_HPP
