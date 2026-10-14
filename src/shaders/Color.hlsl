#ifndef COLOR_HLSL
#define COLOR_HLSL

// Conversion between linear and sRGB color spaces
inline float linearToSrgb(float linearColor)
{
    if (linearColor < 0.0031308f)
        return linearColor * 12.92f;
    else
        return 1.055f * float(pow(linearColor, 1.0f / 2.4f)) - 0.055f;
}

inline float srgbToLinear(float srgbColor)
{
    if (srgbColor < 0.04045f)
        return srgbColor / 12.92f;
    else
        return float(pow((srgbColor + 0.055f) / 1.055f, 2.4f));
}

float3 linearToSrgb(float3 linearColor)
{
    return float3(linearToSrgb(linearColor.x), linearToSrgb(linearColor.y), linearToSrgb(linearColor.z));
}

float3 srgbToLinear(float3 srgbColor)
{
    return float3(srgbToLinear(srgbColor.x), srgbToLinear(srgbColor.y), srgbToLinear(srgbColor.z));
}

// ================================================
//  Tonemapping
// ================================================

// ACES curve fitting was originally written by Stephen Hill (@self_shadow)
// Source: https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl

// sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT
static const float3x3 ACESInputMat =
{
    { 0.59719, 0.35458, 0.04823 },
    { 0.07600, 0.90834, 0.01566 },
    { 0.02840, 0.13383, 0.83777 }
};

// ODT_SAT => XYZ => D60_2_D65 => sRGB
static const float3x3 ACESOutputMat =
{
    { 1.60475, -0.53108, -0.07367 },
    { -0.10208, 1.10813, -0.00605 },
    { -0.00327, -0.07276, 1.07602 }
};

static float3 RRTAndODTFit(float3 v)
{
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

static float3 aces(float3 color)
{

    color = mul(ACESInputMat, color);

    // Apply RRT and ODT
    color = RRTAndODTFit(color);

    color = mul(ACESOutputMat, color);

    // Clamp to [0, 1]
    color = saturate(color);

    return color;
}

float3 tonemap(const float3 color, const float exposureAdjustment, const bool enableTonemapping)
{
    if (enableTonemapping)
    {
        return linearToSrgb(aces(color * exposureAdjustment));
    }
    else
    {
        return linearToSrgb(color * exposureAdjustment);
    }
}

#endif
