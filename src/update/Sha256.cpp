#include "Sha256.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>

namespace miyoofin {

// -------------------------------------------------------------------
// SHA-256 constants and helpers (FIPS 180-4)
// -------------------------------------------------------------------

static const std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline std::uint32_t rotr(std::uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static inline std::uint32_t ch(std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
    return (x & y) ^ (~x & z);
}

static inline std::uint32_t maj(std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

static inline std::uint32_t bsig0(std::uint32_t x)
{
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

static inline std::uint32_t bsig1(std::uint32_t x)
{
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

static inline std::uint32_t ssig0(std::uint32_t x)
{
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

static inline std::uint32_t ssig1(std::uint32_t x)
{
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

// -------------------------------------------------------------------
// Sha256 implementation
// -------------------------------------------------------------------

Sha256::Sha256() : m_bitLen(0), m_bufLen(0)
{
    m_state[0] = 0x6a09e667;
    m_state[1] = 0xbb67ae85;
    m_state[2] = 0x3c6ef372;
    m_state[3] = 0xa54ff53a;
    m_state[4] = 0x510e527f;
    m_state[5] = 0x9b05688c;
    m_state[6] = 0x1f83d9ab;
    m_state[7] = 0x5be0cd19;
}

void Sha256::processBlock(const unsigned char block[64])
{
    std::uint32_t W[64];
    for (int i = 0; i < 16; i++) {
        W[i] = (std::uint32_t(block[i * 4]) << 24) | (std::uint32_t(block[i * 4 + 1]) << 16) |
               (std::uint32_t(block[i * 4 + 2]) << 8) | (std::uint32_t(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; i++) {
        W[i] = ssig1(W[i - 2]) + W[i - 7] + ssig0(W[i - 15]) + W[i - 16];
    }

    std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    std::uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];

    for (int i = 0; i < 64; i++) {
        std::uint32_t T1 = h + bsig1(e) + ch(e, f, g) + K[i] + W[i];
        std::uint32_t T2 = bsig0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + T1;
        d = c;
        c = b;
        b = a;
        a = T1 + T2;
    }

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::update(const void* data, std::size_t len)
{
    auto* in = static_cast<const unsigned char*>(data);
    std::size_t remaining = len;

    // Fill buffer if partial
    if (m_bufLen > 0) {
        std::size_t space = 64 - m_bufLen;
        std::size_t take = remaining < space ? remaining : space;
        std::memcpy(m_buf + m_bufLen, in, take);
        m_bufLen += take;
        in += take;
        remaining -= take;
        if (m_bufLen == 64) {
            processBlock(m_buf);
            m_bufLen = 0;
        }
    }

    // Process full blocks directly from input
    while (remaining >= 64) {
        processBlock(in);
        in += 64;
        remaining -= 64;
    }

    // Buffer remainder
    if (remaining > 0) {
        std::memcpy(m_buf, in, remaining);
        m_bufLen = remaining;
    }

    m_bitLen += static_cast<std::uint64_t>(len) * 8;
}

void Sha256::final(unsigned char out[32])
{
    // Pad
    std::size_t idx = m_bufLen;
    m_buf[idx++] = 0x80;
    if (idx > 56) {
        while (idx < 64)
            m_buf[idx++] = 0;
        processBlock(m_buf);
        idx = 0;
    }
    while (idx < 56)
        m_buf[idx++] = 0;

    // Append length in bits (big-endian)
    for (int i = 7; i >= 0; i--)
        m_buf[idx++] = static_cast<unsigned char>(m_bitLen >> (i * 8));
    processBlock(m_buf);

    // Output state
    for (int i = 0; i < 8; i++) {
        out[i * 4] = static_cast<unsigned char>(m_state[i] >> 24);
        out[i * 4 + 1] = static_cast<unsigned char>(m_state[i] >> 16);
        out[i * 4 + 2] = static_cast<unsigned char>(m_state[i] >> 8);
        out[i * 4 + 3] = static_cast<unsigned char>(m_state[i]);
    }
}

std::string Sha256::hex(const unsigned char raw[32])
{
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (int i = 0; i < 32; i++) {
        result += digits[(raw[i] >> 4) & 0x0f];
        result += digits[raw[i] & 0x0f];
    }
    return result;
}

// -------------------------------------------------------------------
bool sha256File(const std::string& path, std::string& hexOut, std::string& errorOut,
                const std::atomic<bool>* cancellation)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        errorOut = "cannot open file: " + path;
        return false;
    }

    Sha256 hasher;
    char buf[65536];

    while (f.good()) {
        if (cancellation && cancellation->load(std::memory_order_relaxed)) {
            errorOut = "cancelled";
            return false;
        }
        f.read(buf, sizeof(buf));
        auto n = f.gcount();
        if (n > 0)
            hasher.update(buf, static_cast<std::size_t>(n));
    }

    if (f.bad()) {
        errorOut = "read error: " + path;
        return false;
    }

    unsigned char raw[32];
    hasher.final(raw);
    hexOut = Sha256::hex(raw);
    return true;
}

} // namespace miyoofin
