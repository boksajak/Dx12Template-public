#include "shared.h"
#include "Resources.hlsl"
#include "RNG.hlsl"
#include "RTUtils.hlsl"
#include "RTHelpers.hlsl"
#include "Lights.hlsl"

// Calculates the probability of selecting the specular BRDF over the diffuse BRDF.
float getBrdfProbability(MaterialProperties material, float3 V, float3 shadingNormal)
{
    // Evaluate the Fresnel/BRF contribution using the shading normal. The half-vector is not known
    // yet, so this deliberately uses a directional-hemispherical GGX reflectance approximation.
    const float3 specularF0 = baseColorToSpecularF0(material.baseColor, material.metalness, material.reflectance);
    const float diffuseReflectance = luminance(baseColorToDiffuseReflectance(material.baseColor, material.metalness));
    const float alpha = (material.roughness * material.roughness);
    const float3 specularApproximation = specularGGXReflectanceApprox(specularF0, alpha, max(0.0f, dot(V, shadingNormal)));
    const float specularReflectance = luminance(specularApproximation);

    // Approximate relative contribution of BRDF lobes.
    const float specular = specularReflectance;
    const float diffuse = diffuseReflectance * (1.0f - specularReflectance);
    const float p = specular / max(0.0001f, specular + diffuse);

    // Avoid undersampling the less prominent lobe.
    return clamp(p, 0.1f, 0.9f);
}

// Mixture density for MIS, including the probability of choosing each lobe. Delta reflection
// has no continuous density and is handled separately when the next ray hits a light.
float getBrdfPdfForMIS(MaterialProperties material, float3 V, float3 N, float3 L)
{
    // Handle invalid configurations
    if (dot(N, V) <= 0.0f || dot(N, L) <= 0.0f) return 0.0f;
    if (material.metalness == 1.0f && material.roughness == 0.0f) return 0.0f;
    
    const BrdfData data = prepareBRDFData(N, L, V, material);
    const float pSpecular = getBrdfProbability(material, V, N);
    const float specular = (material.roughness == 0.0f) ? 0.0f : specularPdf(data.alpha, data.alphaSquared, data.NdotH, data.NdotV, data.LdotH);
    
    return pSpecular * specular + (1.0f - pSpecular) * diffusePdf(data.NdotL);
}

float getMISWeight(float pdf, float otherPdf)
{
    if (pdf <= 0.0f) return 0.0f;

    // Balance heuristic for MIS    
    return pdf / (pdf + otherPdf);
}

// =========================================================================
//   Path Tracer Pass
// =========================================================================

[shader("raygeneration")]
void RayGenPathTracer()
{
    uint2 LaunchIndex = DispatchRaysIndex().xy;
    uint2 LaunchDimensions = DispatchRaysDimensions().xy;

    // Initialize random numbers generator
    uint rngState = initRNG(LaunchIndex, LaunchDimensions, gData.frameNumber);
    
    // Generate primary ray
    RayDesc ray = generatePrimaryRay(rngState, float2(LaunchIndex), LaunchDimensions);
    
    RayPayload payload = (RayPayload) 0;

    // Book-keeping things for MIS
    float previousBrdfPdf = 0.0f;
    float3 previousPosition = 0.0f;
    bool previousDelta = true;
    bool previousNEESupported = false;

	// Initialize path tracing data
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);
	
	// Start the ray tracing loop
    for (int bounce = 0; bounce < gData.maxBounces; bounce++)
    {
		// Trace the ray
        TraceRayWrapperStandard(ray, payload, rngState, false);

		// On a miss, load the sky value and break out of the ray tracing loop
        if (!payload.hasHit())
        {
            radiance += throughput * getSkyValue(ray.Direction);
            break;
        }

		// Load material properties at the hit point
        const MaterialProperties material = evaluateMaterial(payload);
        
        // View vector for shading
        float3 V = -ray.Direction;
        
		// Account for emissive surfaces
        if (!payload.normalFlipped) //< Only if light was not backfacing
        {
            float emissionWeight = 1.0f;
            
            // Do NEE if we can    
            if (gData.enableNEE && !previousDelta && previousNEESupported && gData.triangleLightCount > 0)
            {
                // Adjust emission weight using MIS if enabled
                if (gData.enableMIS)
                {
                    // Calculate PDFs and apply MIS weight
                    const float3 lightVector = payload.hitPosition - previousPosition;
                    const float lightDistance = length(lightVector);
                    const float lightPdf = getLightPdfForMIS(payload.hitTriangleArea, payload.geometryNormal, lightVector / max(lightDistance, 1e-20f), lightDistance);
                    
                    emissionWeight = (lightPdf > 0.0f) ? getMISWeight(previousBrdfPdf, lightPdf) : 1.0f;
                }
                else
                {
                    emissionWeight = 0.0f;
                }
            }
            
            radiance += throughput * material.emissive * emissionWeight;
        }

		// Evaluate direct light (next event estimation), start by sampling one light 
        if (gData.enableNEE)
        {
            TriangleLight light;
            float3 lightBarycentrics;
            float lightWeight;
            if (sampleLightRIS(rngState, payload.hitPosition, payload.geometryNormal, light, lightBarycentrics, lightWeight))
            {
                // Prepare data needed to evaluate the light
                float3 L;
                float lightDistance;
                getTriangleLightData(light, lightBarycentrics, payload.hitPosition, L, lightDistance);
                const float3 lightIntensity = getTriangleLightIntensity(light, lightBarycentrics, L, lightDistance);

                // Cast shadow ray towards the selected light
                if (castTriangleLightShadowRay(light, lightBarycentrics, offsetRay(payload.hitPosition, payload.geometryNormal), rngState, false))
                {
                    float misWeight = 1.0f;
                    
                    // Apply MIS weighting if enabled (except for last bounce which won't have BRDF sample)
                    if (gData.enableMIS && (bounce < gData.maxBounces - 1))
                    {
                        const float lightPdf = getLightPdfForMIS(light.surfaceArea, light.geometryNormal, L, lightDistance);
                        const float brdfPdf = getBrdfPdfForMIS(material, V, payload.shadingNormal, L);
                        misWeight = getMISWeight(lightPdf, brdfPdf);
                    }
                    
                    // A zero-roughness specular lobe is delta, ignore specular shading in that case
                    const bool isDelta = (material.roughness == 0.0f);
                    
                    // Shade the selected light if unoccluded
                    radiance += throughput * evalCombinedBRDF(payload.shadingNormal, L, V, material, !isDelta) * lightIntensity * lightWeight * misWeight;
                }
            }
        }

		// Terminate loop early on last bounce (we don't need to sample BRDF)
        if (bounce == gData.maxBounces - 1)
            break;

		// Russian Roulette
        if (bounce > MIN_BOUNCES)
        {
            float rrProbability = min(0.95f, luminance(throughput));
            if (rrProbability < rand(rngState))
                break;
            else
                throughput /= rrProbability;
        }

		// Sample BRDF to generate the next ray
		// First, figure out whether to sample diffuse or specular BRDF
        int brdfType;
        if (material.metalness == 1.0f && material.roughness == 0.0f)
        {
			// Fast path for mirrors
            brdfType = SPECULAR_TYPE;
        }
        else
        {

			// Decide whether to sample diffuse or specular BRDF (based on Fresnel term)
            float brdfProbability = getBrdfProbability(material, V, payload.shadingNormal);

            if (rand(rngState) < brdfProbability)
            {
                brdfType = SPECULAR_TYPE;
                throughput /= brdfProbability;
            }
            else
            {
                brdfType = DIFFUSE_TYPE;
                throughput /= (1.0f - brdfProbability);
            }
        }

		// Run importance sampling of selected BRDF to generate reflecting ray direction
        float3 brdfWeight;
        float2 u = float2(rand(rngState), rand(rngState));
        if (!evalIndirectCombinedBRDF(u, payload.shadingNormal, payload.geometryNormal, V, material, brdfType, ray.Direction, brdfWeight))
        {
            break; // Ray was eaten by the surface :(
        }

		// Account for surface properties using the BRDF "weight"
        throughput *= brdfWeight;

        // Remember book-keeping data for MIS
        previousDelta = ((brdfType == SPECULAR_TYPE) && (material.roughness == 0.0f));
        previousNEESupported = isLightSamplingDirectionSupported(payload.geometryNormal, ray.Direction);
        
        if (gData.enableNEE && gData.enableMIS && !previousDelta)
        {
            previousBrdfPdf = getBrdfPdfForMIS(material, V, payload.shadingNormal, ray.Direction);
            previousPosition = payload.hitPosition;
        }

		// Offset a new ray origin from the hitpoint to prevent self-intersections
        ray.Origin = offsetRay(payload.hitPosition, payload.geometryNormal);
    }

	// Write result to screen
    accumulateRadiance(LaunchIndex, radiance);
}