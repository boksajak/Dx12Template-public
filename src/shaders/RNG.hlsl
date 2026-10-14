
// -------------------------------------------------------------------------
//    RNG
// -------------------------------------------------------------------------

// 32-bit Xorshift random number generator
inline uint xorshift32(inout uint rngState)
{
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}

// Jenkins's "one at a time" hash function
uint jenkinsHash(uint x)
{
    x += x << 10;
    x ^= x >> 6;
    x += x << 3;
    x ^= x >> 11;
    x += x << 15;
    return x;
}

// Wang Hash
// Source: https://web.archive.org/web/20070108113114/http://www.cris.com:80/~Ttwang/tech/inthash.htm
uint wangHash(uint key)
{
    key = ~key + (key << 15);
    key = key ^ (key >> 12);
    key = key + (key << 2);
    key = key ^ (key >> 4);
    key = key * 2057;
    key = key ^ (key >> 16);
    return key;
}

// PCG Hash Function
// Source: https://jcgt.org/published/0009/03/02/
uint pcgHash(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Converts unsigned integer into float int range <0; 1) by using 23 most significant bits for mantissa
// Explanation: https://vectrx.substack.com/p/lcg-xs-fast-gpu-rng
inline float uintToFloat(const uint x)
{
    return asfloat(0x3f800000 | (x >> 9)) - 1.0f;
}

uint initRNG(const uint linearIndex, const uint frameNumber) {
    return pcgHash(linearIndex ^ pcgHash(frameNumber));
}

// Initialize RNG for given pixel and frame
uint initRNG(const uint2 pixelCoords, const uint2 resolution, const uint frameNumber) {
    return initRNG(dot(pixelCoords, uint2(1, resolution.x)), frameNumber);
}

// Shared seed for camera jitter and primary-ray opacity
// Use this seed to reconstruct primary ray in subsequent passes
uint initRNGPrimaryRays(const uint2 pixelCoords, const uint2 resolution, const uint frameNumber)
{
    return initRNG(pixelCoords, resolution, frameNumber);
}

// Standard seeed for other uses than primary ray
uint initRNGStandard(const uint2 pixelCoords, const uint2 resolution, const uint frameNumber)
{
    return initRNG(pixelCoords, resolution, pcgHash(frameNumber));
}

// Generate random float in <0; 1) range
inline float rand(inout uint rngState) {
    return uintToFloat(xorshift32(rngState));
}

// Generate a random float in the range <-x; x)
float randInRange(inout uint rng, float x)
{
    return (rand(rng) * 2.0f - 1.0f) * x;
}
