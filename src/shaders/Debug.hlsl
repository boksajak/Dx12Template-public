#ifndef DEBUG_HLSL
#define DEBUG_HLSL

#include "shared.h"
#include "Resources.hlsl"
#include "GBuffer.hlsl"
#include "RNG.hlsl"
#include "RTUtils.hlsl"


// Maps integers to colors using the hash function (generates pseudo-random colors)
float3 hashAndColor(const uint i)
{
    uint hash = jenkinsHash(i);
    float r = ((hash >> 0) & 0xFF) / 255.0f;
    float g = ((hash >> 8) & 0xFF) / 255.0f;
    float b = ((hash >> 16) & 0xFF) / 255.0f;
    return float3(r, g, b);
}

[numthreads(8, 8, 1)]
void DebugVisualization(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    if (pixel.x >= gData.outputWidth || pixel.y >= gData.outputHeight) return;

    if (gData.debugView == DEBUG_VIEW_MOTION_VECTORS)
    {
        // Display signed X/Y motion in red/green
        const float2 motion = gBufferMotionBarycentrics.Load(int3(pixel, 0)).xy - 0.5f;
        Output[pixel] = float4(saturate(0.5f + motion / 32.0f), 0.0f, 1.0f);
        return;
    }

    const GBuffer surface = loadGBuffer(pixel);
    float3 color = 0.0f;
    if (surface.hasHit())
    {
        switch (gData.debugView)
        {
        case DEBUG_VIEW_UV:
            color = float3(frac(surface.uv), 0.0f);
            break;
        case DEBUG_VIEW_SHADING_NORMAL:
            color = surface.shadingNormal * 0.5f + 0.5f;
            break;
        case DEBUG_VIEW_GEOMETRY_NORMAL:
            color = surface.geometryNormal * 0.5f + 0.5f;
            break;
        case DEBUG_VIEW_INSTANCE_ID:
                color = hashAndColor(surface.instanceID);
            break;
        case DEBUG_VIEW_GEOMETRY_ID:
                color = hashAndColor(surface.geometryID);
            break;
        case DEBUG_VIEW_TRIANGLE_ID:
                color = hashAndColor(pcgHash(surface.geometryID) ^ surface.triangleID);
            break;
        default:
            const MaterialProperties material = evaluateMaterial(surface);
            if (gData.debugView == DEBUG_VIEW_ROUGHNESS) color = material.roughness.xxx;
            else if (gData.debugView == DEBUG_VIEW_METALNESS) color = material.metalness.xxx;
            else if (gData.debugView == DEBUG_VIEW_OPACITY) color = material.opacity.xxx;
            else color = linearToSrgb(material.baseColor);
            break;
        }
    }

    // Diagnostic colors are already display values; preserve them without exposure or tonemapping.
    Output[pixel] = float4(accumulateRadiance(pixel, color), 1.0f);
}

#endif