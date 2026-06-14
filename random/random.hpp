#pragma once

#include <cstdint>
#include <ctime>
#include <random>

inline uint32_t InitSeed()
{
    std::random_device rd;
    uint32_t s = rd() ^ uint32_t(std::time(nullptr));
    return s ? s : 1u;
}

inline uint32_t FastRand()
{
    static uint32_t x = 0;
    if (!x) x = InitSeed();
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

inline int GetRandInt(int min, int max)
{
    uint32_t r = FastRand();
    return min + int(r % uint32_t(max - min + 1));
}

inline float GetRandFloat(float min, float max)
{
    uint32_t r = FastRand();
    float f = (r >> 8) * (1.0f / 16777216.0f);
    return min + (max - min) * f;
}
