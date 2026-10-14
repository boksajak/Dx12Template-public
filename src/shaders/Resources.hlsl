#ifndef RESOURCES_HLSL
#define RESOURCES_HLSL

// =========================================================================
//   Resources
// =========================================================================

// Renderer constant buffer
cbuffer ShaderDataCB : register(b0)
{
    ShaderData gData;
}

// Output image
RWTexture2D<float4> Output : register(u0);

// G-buffer outputs
RWTexture2D<float4> gBufferPositionOutput : register(u1);
RWTexture2D<float4> gBufferNormalsOutput : register(u2);
RWTexture2D<float2> gBufferUVOutput : register(u3);
RWTexture2D<float4> gBufferMotionBarycentricsOutput : register(u4);
RWTexture2D<uint4> gBufferIDsOutput : register(u5);

// Accumulation buffer
RWTexture2D<float4> accumulationBuffer : register(u6);

// Automatic exposure multiplier
RWTexture2D<float> autoExposureBuffer : register(u7);

// TLAS of our scene
RaytracingAccelerationStructure sceneTLAS : register(t0);

// Geometry
StructuredBuffer<Vertex> vertexBuffer : register(t1);
StructuredBuffer<uint> indexBuffer : register(t2);
StructuredBuffer<InstanceData> instanceDataBuffer : register(t3);
StructuredBuffer<TriangleLight> triangleLights : register(t4);
StructuredBuffer<Material> materials : register(t5);

// G-buffer inputs
Texture2D<float4> gBufferPosition : register(t6);
Texture2D<float4> gBufferNormals : register(t7);
Texture2D<float2> gBufferUV : register(t8);
Texture2D<float4> gBufferMotionBarycentrics : register(t9);
Texture2D<uint4> gBufferIDs : register(t10);

// Scene textures
Texture2D<float4> sceneTextures[] : register(t0, space1);
SamplerState sceneTextureSampler : register(s0);

#endif
