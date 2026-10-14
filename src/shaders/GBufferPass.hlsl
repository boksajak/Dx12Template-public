#include "shared.h"
#include "Resources.hlsl"
#include "GBuffer.hlsl"
#include "RNG.hlsl"
#include "RTUtils.hlsl"
#include "RTHelpers.hlsl"

// =========================================================================
//   Raytraced G-Buffer Pass
// =========================================================================

[shader("raygeneration")]
void RayGenGBuffer()
{
    const uint2 LaunchIndex = DispatchRaysIndex().xy;
    const uint2 LaunchDimensions = DispatchRaysDimensions().xy;

    // Initialize random numbers generator
    uint rngStatePrimaryRay = initRNGPrimaryRays(LaunchIndex, LaunchDimensions, gData.frameNumber);

    // Initialize the primary ray
    RayDesc ray = generatePrimaryRay(rngStatePrimaryRay, float2(LaunchIndex), LaunchDimensions);
    RayPayload rayPayload = (RayPayload)0;

    // Cast a primary ray
    TraceRayWrapperStandard(ray, rayPayload, rngStatePrimaryRay, false);

    // Initialize G-Buffer entry
    GBuffer gBufferEntry = (GBuffer)0;

    if (!rayPayload.hasHit())
    {
        // Write miss to the G-Buffer
        gBufferEntry.setMiss();
        gBufferEntry.motionVector = calculateMotionVector(float4(ray.Direction, 0.0f), LaunchDimensions);
        storeGBuffer(LaunchIndex, gBufferEntry);
        return;
    }

    // Write hit data into G-Buffer
    gBufferEntry.fromRayPayload(rayPayload);
    gBufferEntry.motionVector = calculateMotionVector(float4(rayPayload.hitPosition, 1.0f), LaunchDimensions);
    
    storeGBuffer(LaunchIndex, gBufferEntry);
}
