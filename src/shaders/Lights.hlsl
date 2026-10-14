#ifndef LIGHTS_HLSL
#define LIGHTS_HLSL

float3 getTriangleLightPosition(const TriangleLight light, const float3 barycentrics)
{
    return light.vertexPosition[0] * barycentrics.x + 
           light.vertexPosition[1] * barycentrics.y + 
           light.vertexPosition[2] * barycentrics.z;
}

void getTriangleLightData(const TriangleLight light, const float3 barycentrics, const float3 hitPosition, out float3 lightDirection, out float lightDistance)
{
    const float3 lightPosition = getTriangleLightPosition(light, barycentrics);
    const float3 lightVector = lightPosition - hitPosition;
    
    lightDistance = length(lightVector);
    lightDirection = lightVector / lightDistance;
}

// Visibility uses the segment between offset endpoints, while lighting&shading uses the original positions.
bool castTriangleLightShadowRay(const TriangleLight light, const float3 barycentrics, const float3 hitOffsetPosition, inout uint rngState, const bool useTraceRayInline)
{
    const float3 lightPosition = getTriangleLightPosition(light, barycentrics);
    const float3 shadowVector = offsetRay(lightPosition, light.geometryNormal) - hitOffsetPosition;
    
    const float shadowDistance = length(shadowVector);
    if (shadowDistance <= 0.0f) return false;
    
    const float3 directionToLight = shadowVector / shadowDistance;
    
    // Exclude the endpoint even if the offset is lost when rounding the segment length.
    const float shadowTMax = nextFloatDownPositive(shadowDistance);
    
    return castShadowRay(hitOffsetPosition, directionToLight, shadowTMax, rngState, useTraceRayInline);
}

float3 getTriangleLightRadiance(const TriangleLight light, const float3 barycentrics)
{
    float3 radiance = light.emissiveFactor;
    if (light.emissiveTextureIndex != INVALID_ID)
    {
        const float2 uv =
            light.vertexUV[0] * barycentrics.x +
            light.vertexUV[1] * barycentrics.y +
            light.vertexUV[2] * barycentrics.z;
        
        radiance *= sceneTextures[NonUniformResourceIndex(light.emissiveTextureIndex)].SampleLevel(sceneTextureSampler, uv, 0.0f).rgb;
    }

    return radiance;
}

float3 getTriangleLightIntensity(const TriangleLight light, const float3 barycentrics, const float3 lightDirection, const float lightDistance)
{
    // Triangle lights emit only through the side pointed to by their geometry normal.
    const float lightCosine = -dot(light.geometryNormal, lightDirection);
    if (lightCosine <= 0.0f) return 0.0f;

    const float geometryTerm = light.surfaceArea * lightCosine / (lightDistance * lightDistance);
    return getTriangleLightRadiance(light, barycentrics) * geometryTerm;
}

bool sampleLightUniform(inout uint rngState, out TriangleLight light, out float3 barycentrics, out float lightSampleWeight)
{
    if (gData.triangleLightCount == 0)
    {
        lightSampleWeight = 0.0f;
        barycentrics = 0.0f;
        return false;
    }

    const uint lightIndex = min(gData.triangleLightCount - 1, uint(rand(rngState) * gData.triangleLightCount));
    light = triangleLights[lightIndex];
    barycentrics = sampleTriangleBarycentrics(rngState);

    lightSampleWeight = float(gData.triangleLightCount);

    return true;
}

// Our emissive lights are single-sided. Reject the backfacing L vectors here
bool isLightSamplingDirectionSupported(float3 geometryNormal, float3 L)
{
    return dot(geometryNormal, L) >= 0.00001f;
}

// Uniform triangle/area density for MIS
float getLightPdfForMIS(float triangleArea, float3 lightNormal, float3 L, float lightDistance)
{
    const float lightCosine = -dot(lightNormal, L);
    if (gData.triangleLightCount == 0 || triangleArea <= 0.0f || lightCosine <= 0.0f) return 0.0f;
    
    return (lightDistance * lightDistance) / max(1e-20f, float(gData.triangleLightCount) * triangleArea * lightCosine);
}

#define RIS_CANDIDATES_LIGHTS 8

bool sampleLightRIS(inout uint rngState, const float3 hitPosition, const float3 geometryNormal, out TriangleLight selectedLight, out float3 selectedBarycentrics, out float lightSampleWeight)
{
    if (gData.triangleLightCount == 0)
    {
        lightSampleWeight = 0.0f;
        selectedBarycentrics = 0.0f;
        return false;
    }

    selectedLight = (TriangleLight)0;
    selectedBarycentrics = 0.0f;
    float totalWeights = 0.0f;
    float selectedTarget = 0.0f;

    for (int i = 0; i < RIS_CANDIDATES_LIGHTS; i++)
    {
        TriangleLight candidate;
        float3 candidateBarycentrics;
        float candidateSampleWeight;
        if (!sampleLightUniform(rngState, candidate, candidateBarycentrics, candidateSampleWeight)) continue;

        float3 lightDirection;
        float lightDistance;
        getTriangleLightData(candidate, candidateBarycentrics, hitPosition, lightDirection, lightDistance);

        // Reject light samples behind the surface before evaluating their RIS target.
        if (!isLightSamplingDirectionSupported(geometryNormal, lightDirection)) continue;

        const float candidateTarget = luminance(getTriangleLightIntensity(candidate, candidateBarycentrics, lightDirection, lightDistance));
        const float candidateRISWeight = candidateTarget * candidateSampleWeight;
        if (candidateRISWeight <= 0.0f) continue;

        totalWeights += candidateRISWeight;
        if (rand(rngState) < candidateRISWeight / totalWeights)
        {
            selectedLight = candidate;
            selectedBarycentrics = candidateBarycentrics;
            selectedTarget = candidateTarget;
        }
    }

    if (totalWeights == 0.0f)
    {
        lightSampleWeight = 0.0f;
        return false;
    }

    lightSampleWeight = (totalWeights / float(RIS_CANDIDATES_LIGHTS)) / selectedTarget;
    return true;
}

#endif
