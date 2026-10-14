#include "shared.h"
#include "Resources.hlsl"
#include "GBuffer.hlsl"
#include "RNG.hlsl"
#include "RTUtils.hlsl"
#include "RTHelpers.hlsl"
#include "AO.hlsl"

[shader("raygeneration")]
void RayGenRaytracedAO()
{
    const uint2 LaunchIndex = DispatchRaysIndex().xy;
    const uint2 LaunchDimensions = DispatchRaysDimensions().xy;

    // Load G-Buffer
    const GBuffer gBufferEntry = loadGBuffer(LaunchIndex);

    if (!gBufferEntry.hasHit())
    {
        accumulateRadiance(LaunchIndex, 1.0f);
        return;
    }

    // Initialize random numbers generator
    uint rngState = initRNGStandard(LaunchIndex, LaunchDimensions, gData.frameNumber);

    const float ao = raytracedAO(rngState, offsetRay(gBufferEntry.hitPosition, gBufferEntry.geometryNormal), gBufferEntry.geometryNormal, gData.aoRadius, 8);

    // Write result to screen
    accumulateRadiance(LaunchIndex, ao.xxx);
}
