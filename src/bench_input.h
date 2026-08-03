// Synthetic input for the benchmark loop.
//
// A throughput benchmark needs *some* bytes to push through the device. There
// is no camera and no decode stage here on purpose: what is being measured is
// what the accelerator and its SDK cost per frame, not what this host's CPU
// costs to prepare one. The pattern is a fixed xorshift sequence rather than
// zeros — plausible data, and identical on every run and every card.
#pragma once

#include <cstddef>
#include <cstdint>

inline void fill_pattern(void* dst, std::size_t bytes) {
    auto* p = static_cast<unsigned char*>(dst);
    std::uint32_t x = 0x2545F491u;
    for (std::size_t i = 0; i < bytes; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        p[i] = static_cast<unsigned char>(x);
    }
}
