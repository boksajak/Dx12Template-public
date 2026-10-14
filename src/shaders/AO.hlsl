
float raytracedAO(inout uint rngState, const float3 hitPosition, const float3 normal, const float radius, const uint samples)
{
    // Build a TBN basis centered around the surface normal
    const float3 r = normalize(float3(rand(rngState), rand(rngState), rand(rngState)) * 2.0f - 1.0f);
    const float3 b1 = normalize(r - normal * dot(r, normal));
    const float3 b2 = cross(normal, b1);
    const float3x3 tbn = float3x3(b1, b2, normal);

    float result = 0.0f;
    for (int i = 0; i < samples; i++)
    {
        float3 randomDir = normalize(float3(rand(rngState) * 2.0f - 1.0f, rand(rngState) * 2.0f - 1.0f, rand(rngState)));
        const float3 direction = mul(randomDir, tbn);

        // Use TraceRay so AO works on DXR Tier 1.0 and respects material opacity.
        if (castShadowRay(hitPosition, direction, radius, rngState, false))
        {
            result += 1.0f;
        }
    }

    return result / float(samples);

}
