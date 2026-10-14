#include "shared.h"
#include "Resources.hlsl"

// Log-luminance histogram: Alex Tardif, "Adaptive Exposure from Luminance Histograms".
// https://alextardif.com/HistogramLuminance.html
// This variant meters a fixed grid in one group, with fractional bins and percentile trimming.
#define METERING_GRID_SIZE 64
#define HISTOGRAM_BINS 256
#define HISTOGRAM_WEIGHT 64
#define METERING_GROUP_SIZE 16
#define METERING_THREADS (METERING_GROUP_SIZE * METERING_GROUP_SIZE)

static const float minLogLuminance = -13.287712f; // log2(1e-4)
static const float maxLogLuminance = 13.287712f;  // log2(1e4)
static const float exposureKey = 0.18f;
static const float adaptationSpeed = 2.0f; // Inverse half-life in seconds (0.5 s).

groupshared uint luminanceHistogram[HISTOGRAM_BINS];

[numthreads(METERING_GROUP_SIZE, METERING_GROUP_SIZE, 1)]
void AutoExposure(uint groupIndex : SV_GroupIndex)
{
    // Clear every bin even if the group size changes.
    for (uint bin = groupIndex; bin < HISTOGRAM_BINS; bin += METERING_THREADS) luminanceHistogram[bin] = 0;
    GroupMemoryBarrierWithGroupSync();

    const uint2 imageSize = uint2(gData.outputWidth, gData.outputHeight);
    for (uint i = groupIndex; i < METERING_GRID_SIZE * METERING_GRID_SIZE; i += METERING_THREADS)
    {
        // Deterministic cell-centre sampling, independent of render resolution.
        const uint2 cell = uint2(i % METERING_GRID_SIZE, i / METERING_GRID_SIZE);
        const uint2 pixel = min(((cell * 2 + 1) * imageSize) / (METERING_GRID_SIZE * 2), imageSize - 1);
        float3 color = accumulationBuffer[pixel].rgb;
        if (!gData.enableTAA) color /= gData.accumulatedFrames;

        // Invalid samples do not vote in the histogram.
        if (!all(isfinite(color))) continue;
        const float luminance = dot(max(color, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
        if (!isfinite(luminance)) continue;

        const float logLuminance = clamp(log2(max(luminance, 1e-4f)), minLogLuminance, maxLogLuminance);
        const float position = saturate((logLuminance - minLogLuminance) / (maxLogLuminance - minLogLuminance)) * (HISTOGRAM_BINS - 1);
        const uint leftBin = uint(position);
        const uint rightBin = min(leftBin + 1, HISTOGRAM_BINS - 1);
        const uint rightWeight = uint(frac(position) * HISTOGRAM_WEIGHT);

        // Each valid sample contributes exactly 64 units, split between adjacent bins.
        InterlockedAdd(luminanceHistogram[leftBin], HISTOGRAM_WEIGHT - rightWeight);
        if (rightWeight != 0) InterlockedAdd(luminanceHistogram[rightBin], rightWeight);
    }
    GroupMemoryBarrierWithGroupSync();

    // One thread reduces the histogram and updates the persistent exposure.
    if (groupIndex != 0) return;

    uint totalWeight = 0;
    for (uint bin = 0; bin < HISTOGRAM_BINS; bin++) totalWeight += luminanceHistogram[bin];

    if (totalWeight == 0)
    {
        // Retain the previous exposure, or initialize to unity if no valid image exists yet.
        if (gData.resetAutoExposure) autoExposureBuffer[uint2(0, 0)] = 1.0f;
        return;
    }

    const float lowWeight = totalWeight * 0.05f;
    const float highWeight = totalWeight * 0.95f;
    float cdf = 0.0f;
    float selectedWeight = 0.0f;
    float logLuminanceSum = 0.0f;
    for (uint bin = 0; bin < HISTOGRAM_BINS; bin++)
    {
        const float nextCdf = cdf + luminanceHistogram[bin];
        // Keep only the overlap with the central 90%, including partial boundary bins.
        const float weight = max(0.0f, min(nextCdf, highWeight) - max(cdf, lowWeight));
        const float logLuminance = lerp(minLogLuminance, maxLogLuminance, float(bin) / float(HISTOGRAM_BINS - 1));
        logLuminanceSum += logLuminance * weight;
        selectedWeight += weight;
        cdf = nextCdf;
    }

    const float averageLuminance = exp2(logLuminanceSum / selectedWeight);
    const float targetExposure = exposureKey / max(averageLuminance, 1e-4f);
    float exposure = targetExposure;

    // Snap on the first enabled frame; never read uninitialized exposure memory.
    if (!gData.resetAutoExposure)
    {
        const float previousExposure = autoExposureBuffer[uint2(0, 0)];
        const float adaptation = 1.0f - exp2(-clamp(gData.deltaTime, 0.0f, 1.0f) * adaptationSpeed);
        exposure = exp2(lerp(log2(previousExposure), log2(targetExposure), adaptation));
    }

    autoExposureBuffer[uint2(0, 0)] = exposure;
}
