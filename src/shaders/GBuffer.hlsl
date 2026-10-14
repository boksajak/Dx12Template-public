#ifndef GBUFFER_HLSL
#define GBUFFER_HLSL

#include "Resources.hlsl"


// Helpers for octahedron encoding of normals
float2 octWrap(float2 v)
{
    return float2((1.0f - abs(v.y)) * (v.x >= 0.0f ? 1.0f : -1.0f), (1.0f - abs(v.x)) * (v.y >= 0.0f ? 1.0f : -1.0f));
}

float2 encodeNormalOctahedron(float3 n)
{
    float2 p = float2(n.x, n.y) * (1.0f / (abs(n.x) + abs(n.y) + abs(n.z)));
    p = (n.z < 0.0f) ? octWrap(p) : p;
    return p;
}

float3 decodeNormalOctahedron(float2 p)
{
    float3 n = float3(p.x, p.y, 1.0f - abs(p.x) - abs(p.y));
    float2 tmp = (n.z < 0.0f) ? octWrap(float2(n.x, n.y)) : float2(n.x, n.y);
    n.x = tmp.x;
    n.y = tmp.y;
    return normalize(n);
}

float4 encodeNormals(float3 geometryNormal, float3 shadingNormal)
{
    return float4(encodeNormalOctahedron(geometryNormal), encodeNormalOctahedron(shadingNormal));
}

void decodeNormals(float4 encodedNormals, out float3 geometryNormal, out float3 shadingNormal)
{
    geometryNormal = decodeNormalOctahedron(encodedNormals.xy);
    shadingNormal = decodeNormalOctahedron(encodedNormals.zw);
}

// Keep surface code independent of storage. Inlining allows unused field loads to be eliminated.
GBuffer loadGBuffer(uint2 pixel)
{
    GBuffer surface = (GBuffer)0;
    
    // Load IDs
    const uint3 ids = gBufferIDs.Load(int3(pixel, 0)).xyz;
    surface.instanceID = ids.x;
    surface.geometryID = ids.y;
    surface.triangleID = ids.z;
    
    // Load motion vectors and barys
    const float4 motionBarycentrics = gBufferMotionBarycentrics.Load(int3(pixel, 0));
    surface.motionVector = motionBarycentrics.xy;
    surface.barys = motionBarycentrics.zw;
    
    // Load hit position
    surface.hitPosition = gBufferPosition.Load(int3(pixel, 0)).xyz;
    
    // Load UVs
    surface.uv = gBufferUV.Load(int3(pixel, 0));
    
    // Decode normals if available
    if (surface.hasHit())
    {
        const float4 encodedNormals = gBufferNormals.Load(int3(pixel, 0));
        decodeNormals(encodedNormals, surface.geometryNormal, surface.shadingNormal);
    }
    
    return surface;
}

void storeGBuffer(uint2 pixel, GBuffer surface)
{
    // Encode normals if available
    float4 normals = 0.0f;
    if (surface.hasHit())
    {
        normals = encodeNormals(surface.geometryNormal, surface.shadingNormal);
    }
    
    gBufferPositionOutput[pixel] = float4(surface.hitPosition, 0.0f);
    gBufferNormalsOutput[pixel] = normals;
    gBufferUVOutput[pixel] = surface.uv;
    gBufferMotionBarycentricsOutput[pixel] = float4(surface.motionVector, surface.barys);
    gBufferIDsOutput[pixel] = uint4(surface.instanceID, surface.geometryID, surface.triangleID, 0);
}

#endif
