#ifndef RTUTILS_HLSL
#define RTUTILS_HLSL

// Default to TraceRay when no device capability define is supplied.
#ifndef SUPPORTS_INLINE_RAY_TRACING
#define SUPPORTS_INLINE_RAY_TRACING 0
#endif
#include "brdf.h"
#include "Color.hlsl"

float3 getSkyValue(const float3 direction)
{
    const float skyIntensity = 10.0f;
    return float3(0.8f, 0.8f, 0.8f) * skyIntensity;
}

RayDesc generatePrimaryRay(inout uint rngState, float2 pixel, const float2 resolution)
{
    // Antialiasing (optional)
    if (gData.enableAntiAliasing)
    {
		// Add a random offset to the pixel 's screen coordinates .
        float2 offset = float2(rand(rngState), rand(rngState));
        pixel += lerp(-0.5.xx, 0.5.xx, offset);
    }
    
    // Convert pixel position to NDC coordinates
    const float2 ndcPixelPos = (((pixel + 0.5f) / resolution) * 2.0f - 1.0f);

    // Initialize ray, extract camera position from view matrix
    RayDesc ray;
    ray.Origin = gData.view[3].xyz;
    ray.TMin = 0.0f;
    ray.TMax = FLT_MAX;

    // Use view matrix to generate ray direction through given pixel
    ray.Direction = normalize(
        mul(float4(ndcPixelPos.x * gData.tanHalfFovY * gData.aspectRatio,
            -ndcPixelPos.y * gData.tanHalfFovY,
            1, 0),
            gData.view
        ).xyz);

    return ray;
}

// Project a static world position (w = 1) or sky direction (w = 0) into both frames.
// The half-pixel bias lets integerPixel + motionVector address the previous pixel centre.
float2 calculateMotionVector(float4 worldPosition, float2 resolution)
{
    const float4 clip = mul(gData.viewProj, worldPosition);
    const float4 previousClip = mul(gData.previousViewProj, worldPosition);
    
    // Map invalid values to "no movement"
    if (!all(isfinite(clip)) || !all(isfinite(previousClip)) || clip.w <= 1e-6f || previousClip.w <= 1e-6f) return 0.5f;

    const float2 uv = (clip.xy / clip.w) * float2(0.5f, -0.5f) + 0.5f;
    const float2 previousUv = (previousClip.xy / previousClip.w) * float2(0.5f, -0.5f) + 0.5f;
    const float2 motionVector = (previousUv - uv) * resolution + 0.5f;
    
    return all(isfinite(motionVector)) ? motionVector : 0.5f;
}

// Returns the next representable float below value. Requires a positive, finite input.
// Positive IEEE-754 float bit patterns are ordered, so subtracting one steps down by one ULP.
// asuint/asfloat reinterpret the bits; they do not perform numeric conversions.
float nextFloatDownPositive(const float value)
{
    return asfloat(asuint(value) - 1u);
}

// Offsets the point away from the surface along the normal, so that self-intersection cannot occur
// Source: Ray Tracing Gems "A Fast and Robust Method for Avoiding Self-Intersection"
float3 offsetRay(const float3 p, const float3 n)
{
    const float origin = 1.0f / 32.0f;
    const float float_scale = 1.0f / 65536.0f;
    const float int_scale = 256.0f;

    int3 of_i = int3(int_scale * n.x, int_scale * n.y, int_scale * n.z);
    
    float3 p_i = float3(
        asfloat(asint(p.x) + ((p.x < 0) ? -of_i.x : of_i.x)),
        asfloat(asint(p.y) + ((p.y < 0) ? -of_i.y : of_i.y)),
        asfloat(asint(p.z) + ((p.z < 0) ? -of_i.z : of_i.z)));

    return float3(abs(p.x) < origin ? p.x + float_scale * n.x : p_i.x,
        abs(p.y) < origin ? p.y + float_scale * n.y : p_i.y,
        abs(p.z) < origin ? p.z + float_scale * n.z : p_i.z);
}

float3 normalizedToWorldSpace(const float3 normalizedPosition)
{
    return gData.sceneMin + gData.sceneScale * normalizedPosition;
}

float3 worldSpaceToNormalized(const float3 worldSpacePosition)
{
    return (worldSpacePosition - gData.sceneMin) / gData.sceneScale;
}

float3 unpackInputPosition(const float3 normalizedPosition)
{
    return normalizedToWorldSpace(normalizedPosition);
}

float3 packInputPosition(const float3 worldSpacePosition)
{
    return worldSpaceToNormalized(worldSpacePosition);
}

uint linearizePixelPos(uint2 pixelPos, uint frameWidth)
{
    return pixelPos.y * frameWidth + pixelPos.x;
}

// Uniform triangle sampling via the square-root warp
// Source: "Generating Random Points in Triangles" Graphics Gems (1990) by Turk
float3 sampleTriangleBarycentrics(inout uint rngState)
{
    const float sqrtU = sqrt(rand(rngState));
    const float v = rand(rngState);
    return float3(1.0f - sqrtU, sqrtU * (1.0f - v), sqrtU * v);
}

void processHit(inout RayPayload payload, const uint primitiveIndex, const float3x4 objectToWorld, const float3x4 worldToObject, const float2 attribBarycentrics, const uint instanceID, const uint instanceIndex, const float3 worldRayDirection)
{
    // Load indices from index buffer
    const InstanceData instance = instanceDataBuffer[instanceIndex];
    const uint indexOffset = instance.indexOffset + primitiveIndex * 3;
    const uint3 indices = uint3(
        indexBuffer[NonUniformResourceIndex(indexOffset + 0)],
        indexBuffer[NonUniformResourceIndex(indexOffset + 1)],
        indexBuffer[NonUniformResourceIndex(indexOffset + 2)]) + instance.vertexOffset;

    // Load vertices of hit triangle and transform them to world space
    const Vertex objectA = vertexBuffer[indices.x];
    const Vertex objectB = vertexBuffer[indices.y];
    const Vertex objectC = vertexBuffer[indices.z];
    
    const float3 worldA = mul(objectToWorld, float4(objectA.position, 1.0f));
    const float3 worldB = mul(objectToWorld, float4(objectB.position, 1.0f));
    const float3 worldC = mul(objectToWorld, float4(objectC.position, 1.0f));

    // Calculate precise hit position
    float3 barycentrics = float3((1.0f - attribBarycentrics.x - attribBarycentrics.y), attribBarycentrics.x, attribBarycentrics.y);
    payload.hitPosition = worldA * barycentrics[0] + worldB * barycentrics[1] + worldC * barycentrics[2];

    // Calculate geometry normal and triangle area
    const float3 edgeBA = worldB - worldA;
    const float3 edgeCA = worldC - worldA;
    const float3 triangleNormal = cross(edgeCA, edgeBA);
    payload.hitTriangleArea = 0.5f * length(triangleNormal);
    payload.geometryNormal = normalize(triangleNormal);

    // Flip the normal towards the ray
    const float3 V = -worldRayDirection;
    if (dot(payload.geometryNormal, V) < 0.0f)
    {
        payload.geometryNormal = -payload.geometryNormal;
        payload.normalFlipped = true;
    }
    else
    {
        payload.normalFlipped = false;
    }

    // Shading normal will be same as geometry, unless its specified per vertex
    payload.shadingNormal = payload.geometryNormal;
    
    // Calculate UVs
    payload.uv = objectA.uv * barycentrics.x + objectB.uv * barycentrics.y + objectC.uv * barycentrics.z;

    // Fetch and transform shading normal to world space
    const float3 objectNormal = objectA.normal * barycentrics.x + objectB.normal * barycentrics.y + objectC.normal * barycentrics.z;
    
    // Row-vector multiplication by worldToObject applies the inverse transpose.
    const float3 worldNormal = mul(objectNormal, (float3x3) worldToObject);
    
    // Set shading normal if available
    const float normalLengthSquared = dot(worldNormal, worldNormal);
    if (normalLengthSquared > 0.0f)
    {
        payload.shadingNormal = worldNormal * rsqrt(normalLengthSquared);
        
        // Flip shading normal to same side as geometry normal
        if (dot(payload.shadingNormal, payload.geometryNormal) < 0.0f)
        {
            payload.shadingNormal = -payload.shadingNormal;
        }
    }

    // Pass hit data via payload
    payload.barys = attribBarycentrics;
    payload.triangleID = primitiveIndex;
    payload.geometryID = instanceID;
    payload.instanceID = instanceIndex;
}

void TraceRayWrapperStandard(const RayDesc ray, inout RayPayload rayPayload, inout uint rngState, const bool useTraceInline)
{
#if SUPPORTS_INLINE_RAY_TRACING
    if (useTraceInline)
    {
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rayQuery;
        rayQuery.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xFF, ray);
        rayQuery.Proceed();
        rayPayload.rngState = rngState;

        if (rayQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        {
            rayPayload.setMiss();
        }
        else
        {
            processHit(rayPayload, rayQuery.CommittedPrimitiveIndex(), rayQuery.CommittedObjectToWorld3x4(), rayQuery.CommittedWorldToObject3x4(), rayQuery.CommittedTriangleBarycentrics(), rayQuery.CommittedInstanceID(), rayQuery.CommittedInstanceIndex(), ray.Direction);
        }
    }
    else
#endif
    {
        rayPayload.rngState = rngState;
        TraceRay(sceneTLAS, RAY_FLAG_NONE, 0xFF, STANDARD_RAY_INDEX, 0, STANDARD_RAY_INDEX, ray, rayPayload);
        rngState = rayPayload.rngState;
    }
}

void TraceRayWrapperShadow(const RayDesc ray, inout ShadowPayload payload, inout uint rngState, const bool useTraceInline)
{
#if SUPPORTS_INLINE_RAY_TRACING
    if (useTraceInline)
    {
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> rayQuery;
        rayQuery.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xFF, ray);
        rayQuery.Proceed();

        payload.rngState = rngState;
        payload.hasHit = rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1u : 0u;
    }
    else
#endif
    {
        payload.rngState = rngState;
        TraceRay(sceneTLAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, SHADOW_RAY_INDEX, 0, SHADOW_RAY_INDEX, ray, payload);
        rngState = payload.rngState;
    }
}

bool castShadowRay(float3 offsetPosition, float3 directionToLight, float TMax, inout uint rngState, const bool useTraceRayInline)
{
    RayDesc ray;
    ray.Origin = offsetPosition;
    ray.Direction = directionToLight;
    ray.TMin = 0.0f;
    ray.TMax = TMax;

    ShadowPayload payload = (ShadowPayload)0;
    TraceRayWrapperShadow(ray, payload, rngState, useTraceRayInline);

    return payload.hasHit == 0;
}

float4 sampleSceneTexture(const int textureIndex, const float2 uv)
{
    if (textureIndex == INVALID_ID)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }
    else
    {
        return sceneTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(sceneTextureSampler, uv, 0.0f);
    }
}

float evaluateOpacity(const Material material, const float2 uv)
{
    return saturate(material.properties.opacity * sampleSceneTexture(material.baseColorTexture, uv).a);
}

MaterialProperties evaluateMaterial(const uint instanceID, const float2 uv)
{
    // Fetch material and itsbasic properties
    const Material material = materials[instanceID];
    MaterialProperties properties = material.properties;

    // Apply textures on top of basic properties
    const float4 baseColor = sampleSceneTexture(material.baseColorTexture, uv);
    properties.baseColor *= baseColor.rgb;
    properties.opacity *= baseColor.a;

    const float4 metallicRoughness = sampleSceneTexture(material.metallicRoughnessTexture, uv);
    properties.roughness *= metallicRoughness.g;
    properties.metalness *= metallicRoughness.b;

    properties.emissive *= sampleSceneTexture(material.emissiveTexture, uv).rgb;

    return properties;
}

MaterialProperties evaluateMaterial(const GBuffer surface)
{
    return evaluateMaterial(surface.instanceID, surface.uv);
}

MaterialProperties evaluateMaterial(const RayPayload hit)
{
    return evaluateMaterial(hit.instanceID, hit.uv);
}

// Store unexposed HDR and return its average for callers that need immediate output.
float3 accumulateRadiance(uint2 pixel, float3 radiance)
{
    if (pixel.x >= gData.outputWidth || pixel.y >= gData.outputHeight)
        return 0.0f;
        
    // TAA consumes current-frame linear HDR instead of a progressive sum.
    if (gData.enableTAA)
    {
        accumulationBuffer[pixel] = float4(radiance, 1.0f);
        return radiance;
    }

    // Temporal accumulation
    float3 previousColor = accumulationBuffer[pixel].rgb;
    float3 accumulatedColor = radiance;
    if (gData.accumulatedFrames > 1)
    {
        accumulatedColor = previousColor + radiance;
    }
    
    accumulationBuffer[pixel] = float4(accumulatedColor, 1.0f);

    return accumulatedColor / gData.accumulatedFrames;
}

#endif
