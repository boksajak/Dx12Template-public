#ifndef RTHELPERS_HLSL
#define RTHELPERS_HLSL

bool testOpacity(const uint instanceID, const uint primitiveIndex, const float2 barycentrics, inout uint rngState)
{
    const Material material = materials[instanceID];
    if (material.alphaMode == ALPHA_MODE_OPAQUE) return false;

    float2 uv = 0.0f;
    if (material.baseColorTexture != INVALID_ID)
    {
        // Figure out exact UVs
        const InstanceData instance = instanceDataBuffer[instanceID];
        
        const uint indexOffset = instance.indexOffset + primitiveIndex * 3;
        const uint3 indices = 
            uint3(indexBuffer[indexOffset + 0],
                  indexBuffer[indexOffset + 1],
                  indexBuffer[indexOffset + 2]) + instance.vertexOffset;
        
        uv = vertexBuffer[indices.x].uv * (1.0f - barycentrics.x - barycentrics.y) + vertexBuffer[indices.y].uv * barycentrics.x + vertexBuffer[indices.z].uv * barycentrics.y;
    }

    // Fetch exact opacity for this hit
    const float opacity = evaluateOpacity(material, uv);
    
    // Stochastic opacity hit
    if (material.alphaMode == ALPHA_MODE_MASK) return (opacity < material.alphaCutoff);
    if (opacity <= 0.0f) return true;
    if (opacity >= 1.0f) return false;
    
    return rand(rngState) >= opacity;
}

[shader("anyhit")]
void AnyHit(inout RayPayload payload : SV_RayPayload, BuiltInTriangleIntersectionAttributes attributes : SV_IntersectionAttributes)
{
    if (testOpacity(InstanceIndex(), PrimitiveIndex(), attributes.barycentrics, payload.rngState))
    {
        IgnoreHit();
    }
}

[shader("anyhit")]
void ShadowAnyHit(inout ShadowPayload payload : SV_RayPayload, BuiltInTriangleIntersectionAttributes attributes : SV_IntersectionAttributes)
{
    if (testOpacity(InstanceIndex(), PrimitiveIndex(), attributes.barycentrics, payload.rngState))
    {
        IgnoreHit();
    }
    else
    {
        AcceptHitAndEndSearch();
    }
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload : SV_RayPayload, BuiltInTriangleIntersectionAttributes attributes : SV_IntersectionAttributes)
{
    processHit(payload, PrimitiveIndex(), ObjectToWorld3x4(), WorldToObject3x4(), attributes.barycentrics, InstanceID(), InstanceIndex(), WorldRayDirection());
}

[shader("miss")]
void Miss(inout RayPayload payload : SV_RayPayload)
{
    payload.setMiss();
}

[shader("closesthit")]
void ShadowClosestHit(inout ShadowPayload payload : SV_RayPayload, BuiltInTriangleIntersectionAttributes attributes : SV_IntersectionAttributes)
{
    payload.hasHit = 1;
}

[shader("miss")]
void ShadowMiss(inout ShadowPayload payload : SV_RayPayload)
{
    payload.hasHit = 0;
}

#endif