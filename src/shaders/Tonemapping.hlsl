#include "shared.h"
#include "Resources.hlsl"
#include "Color.hlsl"

[numthreads(8, 8, 1)]
void Tonemapping(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    if (pixel.x >= gData.outputWidth || pixel.y >= gData.outputHeight) return;

    float3 color = accumulationBuffer[pixel].rgb;
    if (!gData.enableTAA) color /= gData.accumulatedFrames;

    // AO is a visibility value, not radiance: display it directly after accumulation or TAA.
    if (gData.outputAO)
    {
        Output[pixel] = float4(color, 1.0f);
        return;
    }

    float exposure = gData.exposureAdjustment;
    if (gData.enableAutoExposure) exposure *= autoExposureBuffer[uint2(0, 0)];

    Output[pixel] = float4(tonemap(color, exposure, gData.enableTonemapping), 1.0f);
}
