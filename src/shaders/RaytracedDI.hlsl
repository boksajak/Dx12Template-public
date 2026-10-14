#include "shared.h"
#include "Resources.hlsl"
#include "GBuffer.hlsl"
#include "RNG.hlsl"
#include "RTUtils.hlsl"
#include "RTHelpers.hlsl"
#include "Lights.hlsl"

[shader("raygeneration")]
void RayGenRaytracedDI()
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

    // Regenerate the same primary ray as the G-buffer pass.
    uint rngStatePrimaryRay = initRNGPrimaryRays(LaunchIndex, LaunchDimensions, gData.frameNumber);
    const RayDesc primaryRay = generatePrimaryRay(rngStatePrimaryRay, float2(LaunchIndex), LaunchDimensions);
    
    // Fetch material
    const MaterialProperties material = evaluateMaterial(gBufferEntry);

    float3 radiance = material.emissive;

    TriangleLight light;
    float3 lightBarycentrics;
    float lightWeight;
    if (sampleLightRIS(rngState, gBufferEntry.hitPosition, gBufferEntry.geometryNormal, light, lightBarycentrics, lightWeight))
    {
        // Prepare data needed to evaluate the light
        float3 L;
        float lightDistance;
        getTriangleLightData(light, lightBarycentrics, gBufferEntry.hitPosition, L, lightDistance);
        const float3 lightIntensity = getTriangleLightIntensity(light, lightBarycentrics, L, lightDistance);

        // Cast shadow ray towards the selected light
        if (castTriangleLightShadowRay(light, lightBarycentrics, offsetRay(gBufferEntry.hitPosition, gBufferEntry.geometryNormal), rngState, false))
        {
            // Figure out V vector for shading
            const float3 viewDirection = -primaryRay.Direction;
            
            // Shade the selected light if unoccluded
            radiance += evalCombinedBRDF(gBufferEntry.shadingNormal, L, viewDirection, material) * lightIntensity * lightWeight;
        }
    }

    // Write result to screen
    accumulateRadiance(LaunchIndex, radiance);
}
