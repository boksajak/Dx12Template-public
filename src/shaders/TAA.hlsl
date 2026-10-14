#include "shared.h"

// Temporal resolve framework: Brian Karis, "High Quality Temporal Supersampling" (SIGGRAPH 2014).
// https://advances.realtimerendering.com/s2014/

cbuffer TAAConstants : register(b0)
{
    uint frameWidth;
    uint frameHeight;
    uint resetHistory;
}

Texture2D<float4> currentColor : register(t0);
Texture2D<float4> historyColor : register(t1);
Texture2D<float4> motionBarycentrics : register(t2);
Texture2D<float4> resolvedColor : register(t3);
RWTexture2D<float4> resolvedOutput : register(u0);
RWTexture2D<float4> historyOutput : register(u1);
RWTexture2D<float4> hdrOutput : register(u2);
SamplerState linearClamp : register(s0);

// YCgCo neighborhood constraints: Lasse Jon Fuglsang Pedersen, "Temporal Reprojection Anti-Aliasing in INSIDE" (GDC 2016).
// https://github.com/playdeadgames/temporal/blob/master/GDC2016_Temporal_Reprojection_AA_INSIDE.pdf
// Channel order here is Y, Cg, Co.
float3 rgbToYCgCo(float3 rgb)
{
    return float3(dot(rgb, float3(0.25f, 0.5f, 0.25f)), dot(rgb, float3(-0.25f, 0.5f, -0.25f)), dot(rgb, float3(0.5f, 0.0f, -0.5f)));
}

float3 yCgCoToRGB(float3 color)
{
    const float tmp = color.x - color.y;
    return float3(tmp + color.z, color.x + color.y, tmp - color.z);
}

float3 loadCurrentColor(int2 pixel)
{
    const float3 color = currentColor.Load(int3(pixel, 0)).rgb;
    
    // Keep HDR values within the representable range of the FP16 history buffer.
    return clamp(color, -65504.0f, 65504.0f);
}

// Reference: "Bicubic Filtering in Fewer Taps" (2012), https://vec3.ca/posts/bicubic-filtering-in-fewer-taps
// Catmull-Rom's middle two weights merge into one bilinear tap per axis (3x3 taps).
float3 sampleHistory(float2 pixelPosition)
{
    const float2 centre = floor(pixelPosition - 0.5f) + 0.5f;
    const float2 f = pixelPosition - centre;
    const float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    const float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    const float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    const float2 w3 = f * f * (-0.5f + 0.5f * f);
    const float2 weights[3] = { w0, w1 + w2, w3 };
    const float2 positions[3] = { centre - 1.0f, centre + w2 / (w1 + w2), centre + 2.0f };
    const float2 resolution = float2(frameWidth, frameHeight);
    float3 color = 0.0f;
    
    [unroll]
    for (int y = 0; y < 3; y++)
    {
        [unroll]
        for (int x = 0; x < 3; x++)
        {
            color += historyColor.SampleLevel(linearClamp, float2(positions[x].x, positions[y].y) / resolution, 0.0f).rgb * weights[x].x * weights[y].y;
        }
    }
    return color;
}

[numthreads(16, 16, 1)]
void MainTAA(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    if (pixel.x >= frameWidth || pixel.y >= frameHeight) return;
    
    // Load and check current pixel
    const float3 current = loadCurrentColor(pixel);
    
    // Do not read uninitialized history or variance-clamp the reset frame.
    if (resetHistory)
    {
        resolvedOutput[pixel] = float4(current, 1.0f);
        return;
    }

    // Fetch motion vector
    float2 motion = motionBarycentrics.Load(int3(pixel, 0)).xy;
    
    // Accumulated color and squared color in YCgCo
    float3 mean = 0.0f;
    float3 secondMoment = 0.0f;
    
    // Do a 3x3 neighborhood search to gather color statistics for the history clamp
    // Additionally, select the longest motion vector for history reprojection
    [unroll] for (int y = -1; y <= 1; y++)
    {
        [unroll] for (int x = -1; x <= 1; x++)
        {
            const int2 neighborTap = clamp(int2(pixel) + int2(x, y), int2(0, 0), int2(frameWidth, frameHeight) - 1);
            const float3 color = rgbToYCgCo(loadCurrentColor(neighborTap));
            
            // Get color statistics
            mean += color;
            secondMoment += color * color;
            
            // Find longest vector
            const float2 candidate = motionBarycentrics.Load(int3(neighborTap, 0)).xy;
            
            // Longest-vector dilation: Intel TAA, USE_LONGEST_VELOCITY_VECTOR.
            // https://github.com/GameTechDev/TAA
            // Compare actual displacement, excluding the stored index-to-centre bias.
            if (dot(candidate - 0.5f, candidate - 0.5f) > dot(motion - 0.5f, motion - 0.5f)) motion = candidate;
        }
    }
    
    // Reproject after motion dilation; the clamp sampler handles off-screen history positions.
    const float2 previousPixel = float2(pixel) + motion;
    if (!all(isfinite(previousPixel)))
    {
        resolvedOutput[pixel] = float4(current, 1.0f);
        return;
    }
    
    // Variance bounds: Marco Salvi, "An Excursion in Temporal Supersampling" (GDC 2016).
    // https://developer.download.nvidia.com/gameworks/events/GDC2016/msalvi_temporal_supersampling.pdf
    // Build YCgCo history-clamping bounds from the neighborhood mean +/- one standard deviation.
    mean /= 9.0f;
    const float colorBoxSigma = 1.0f;
    const float3 sigma = sqrt(max(0.0f, secondMoment / 9.0f - mean * mean)); //< per-channel standard deviation
    const float3 colorMin = mean - colorBoxSigma * sigma;
    const float3 colorMax = mean + colorBoxSigma * sigma;
    
    // Grab history from previos frame
    const float3 historyRGB = sampleHistory(previousPixel);
    if (!all(isfinite(historyRGB)))
    {
        resolvedOutput[pixel] = float4(current, 1.0f);
        return;
    }
    
    float3 history = rgbToYCgCo(historyRGB);
    float alpha = 0.1f;
    
    // Anti-flicker trick
    {
        // Local anti-flicker heuristic inspired by Karis (2014)
        const float d = min(abs(colorMin.x - history.x), abs(colorMax.x - history.x));
        const float width = colorMax.x - colorMin.x;
        alpha *= d / max(d + width, 1e-6f);
    }
    
    // Mix history and current if we got past all traps
    history = clamp(history, colorMin, colorMax);
    const float3 result = yCgCoToRGB(lerp(history, rgbToYCgCo(current), alpha));
    
    // Write output, clamp to FP16 bounds
    resolvedOutput[pixel] = float4(clamp(result, -65504.0f, 65504.0f), 1.0f);
}

[numthreads(16, 16, 1)]
void UpdateTAAHistory(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    if (pixel.x >= frameWidth || pixel.y >= frameHeight) return;
    
    const float3 color = resolvedColor.Load(int3(pixel, 0)).rgb;
    
    historyOutput[pixel] = float4(color, 1.0f);
    hdrOutput[pixel] = float4(color, 1.0f);
}
