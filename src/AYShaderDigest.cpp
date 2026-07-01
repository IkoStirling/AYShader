// AYShaderDigest.cpp — SHA-256 for cache keys (Phase 4-I)

#include "detail/AYShaderDigest.h"

#include <cstring>
#include <iomanip>
#include <sstream>

namespace ayt::shader::detail
{

namespace {

constexpr uint32_t kInitialHash[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline uint32_t rotr(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32u - n));
}

inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (~x & z);
}

inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t sig0(uint32_t x)
{
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

inline uint32_t sig1(uint32_t x)
{
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

inline uint32_t gam0(uint32_t x)
{
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

inline uint32_t gam1(uint32_t x)
{
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

void sha256Transform(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24)
             | (static_cast<uint32_t>(block[i * 4 + 1]) << 16)
             | (static_cast<uint32_t>(block[i * 4 + 2]) << 8)
             | static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        w[i] = gam1(w[i - 2]) + w[i - 7] + gam0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + sig1(e) + ch(e, f, g) + kRoundConstants[i] + w[i];
        const uint32_t t2 = sig0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

} // namespace

std::array<uint8_t, 32> sha256(const std::string& data)
{
    uint32_t state[8];
    std::memcpy(state, kInitialHash, sizeof(state));

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.data());
    const uint64_t bitLen = static_cast<uint64_t>(data.size()) * 8u;

    uint64_t offset = 0;
    while (offset + 64 <= data.size()) {
        sha256Transform(state, bytes + offset);
        offset += 64;
    }

    uint8_t block[64] = {};
    const uint64_t remaining = data.size() - offset;
    if (remaining > 0) {
        std::memcpy(block, bytes + offset, static_cast<size_t>(remaining));
    }
    block[remaining] = 0x80;

    if (remaining >= 56) {
        sha256Transform(state, block);
        std::memset(block, 0, sizeof(block));
    }

    for (int i = 0; i < 8; ++i) {
        block[56 + i] = static_cast<uint8_t>((bitLen >> (56 - i * 8)) & 0xFFu);
    }
    sha256Transform(state, block);

    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<uint8_t>((state[i] >> 24) & 0xFFu);
        out[i * 4 + 1] = static_cast<uint8_t>((state[i] >> 16) & 0xFFu);
        out[i * 4 + 2] = static_cast<uint8_t>((state[i] >> 8) & 0xFFu);
        out[i * 4 + 3] = static_cast<uint8_t>(state[i] & 0xFFu);
    }
    return out;
}

std::string sha256Hex(const std::string& data)
{
    const std::array<uint8_t, 32> digest = sha256(data);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : digest) {
        oss << std::setw(2) << static_cast<unsigned>(byte);
    }
    return oss.str();
}

} // namespace ayt::shader::detail
