
#if __cplusplus
#pragma once

	// Typedefs for sharing types between C++ (using GLM) and HLSL
	#include "..\common.h"
	typedef uint32_t uint;

	typedef glm::vec2 float2;
	typedef glm::vec3 float3;
	typedef glm::vec4 float4;

	typedef glm::uvec2 uint2;
	typedef glm::uvec3 uint3;
	typedef glm::uvec4 uint4;

	typedef glm::ivec2 int2;
	typedef glm::ivec3 int3;
	typedef glm::ivec4 int4;

	typedef glm::mat2 float2x2;
	typedef glm::mat3 float3x3;
	typedef glm::mat4 float4x4;
#else
	#define FLT_MAX 3.402823466e+38F
#endif

#include "brdf.h"

#define INVALID_ID -1

#define DEBUG_VIEW_BASE_COLOR 0
#define DEBUG_VIEW_UV 1
#define DEBUG_VIEW_SHADING_NORMAL 2
#define DEBUG_VIEW_GEOMETRY_NORMAL 3
#define DEBUG_VIEW_INSTANCE_ID 4
#define DEBUG_VIEW_GEOMETRY_ID 5
#define DEBUG_VIEW_TRIANGLE_ID 6
#define DEBUG_VIEW_ROUGHNESS 7
#define DEBUG_VIEW_METALNESS 8
#define DEBUG_VIEW_OPACITY 9
#define DEBUG_VIEW_MOTION_VECTORS 10

#define ALPHA_MODE_OPAQUE 0
#define ALPHA_MODE_BLEND 1
#define ALPHA_MODE_MASK 2

struct Material
{
#if __cplusplus
	brdf::MaterialProperties properties =
	{
		float3(1.0f), // baseColor
		0.0f,         // metalness
		float3(0.0f), // emissive
		1.0f,         // roughness
		0.0f,         // transmissivness
		0.5f,         // reflectance (4% dielectric F0)
		1.0f          // opacity
	};
#else
	MaterialProperties properties;
#endif
#if __cplusplus
	int baseColorTexture = INVALID_ID;
	int metallicRoughnessTexture = INVALID_ID;
	int emissiveTexture = INVALID_ID;
	uint alphaMode = ALPHA_MODE_OPAQUE;
	float alphaCutoff = 0.5f;
#else
	int baseColorTexture;
	int metallicRoughnessTexture;
	int emissiveTexture;
	uint alphaMode;
	float alphaCutoff;
#endif
};

#define MIN_BOUNCES 3
#define MAX_BOUNCES 8

struct TriangleLight
{
	float3 vertexPosition[3];
	float3 geometryNormal;
	float surfaceArea;
	float3 emissiveFactor;
	float2 vertexUV[3];
	int emissiveTextureIndex;
};

struct Vertex
{
	float3 position;
	float3 normal;
	float2 uv;
};

struct InstanceData
{
	uint vertexOffset;
	uint indexOffset;
};

struct ShaderData
{
	float4x4 view;
	float4x4 proj;
	float4x4 viewProj;
	float4x4 previousViewProj;

	uint frameNumber;
	uint outputWidth;
	uint outputHeight;
	float deltaTime;

	float3 sceneMin;
	float aspectRatio;
	float3 sceneMax;
	float tanHalfFovY;

	float3 sceneScale;
	float aoRadius;

	uint triangleLightCount;
	uint accumulatedFrames;
	float exposureAdjustment;
	uint enableTonemapping;

	uint enableAntiAliasing;
	uint maxBounces;
	uint enableNEE;
	uint debugView;

	uint enableMIS;
	uint enableTAA;
	uint enableAutoExposure;
	uint resetAutoExposure;

	uint outputAO;
	uint3 padding;
};

#define STANDARD_RAY_INDEX 0
#define SHADOW_RAY_INDEX 1

struct ShadowPayload
{
	uint hasHit;
	uint rngState;
};

struct RayPayload
{
	float3 hitPosition;
	uint instanceID;

	float3 geometryNormal;
	float3 shadingNormal;
	float2 uv;

	uint triangleID;
	uint geometryID;
	float2 barys;

	float hitTriangleArea;
	bool normalFlipped;
	uint rngState;

	bool hasHit() {
		return (instanceID != uint(INVALID_ID));
	}

	void setMiss() {
		instanceID = uint(INVALID_ID);
	}
};

struct NNInput
{
	float3 position;
	uint geometryID;
	uint instanceID;
	uint triangleID;
	float2 barys;
};

struct GBuffer
{
	// Previous minus current screen position in pixels, plus 0.5 (stationary = 0.5).
	float2 motionVector;

	float3 hitPosition;
	float3 geometryNormal;
	float3 shadingNormal;
	float2 uv;

	uint geometryID;
	uint instanceID;
	uint triangleID;
	float2 barys;

	bool hasHit() {
		return (instanceID != uint(INVALID_ID));
	}

	void setMiss() {
		instanceID = uint(INVALID_ID);
	}

	void fromRayPayload(RayPayload rayPayload) {
		hitPosition = rayPayload.hitPosition;
		geometryNormal = rayPayload.geometryNormal;
		shadingNormal = rayPayload.shadingNormal;
		uv = rayPayload.uv;
		geometryID = rayPayload.geometryID;
		instanceID = rayPayload.instanceID;
		triangleID = rayPayload.triangleID;
		barys = rayPayload.barys;
	}

	RayPayload toRayPayload() {
		RayPayload rayPayload;

		rayPayload.hitPosition = hitPosition;
		rayPayload.geometryNormal = geometryNormal;
		rayPayload.shadingNormal = shadingNormal;
		rayPayload.uv = uv;
		rayPayload.geometryID = geometryID;
		rayPayload.instanceID = instanceID;
		rayPayload.triangleID = triangleID;
		rayPayload.barys = barys;

		return rayPayload;
	}

	NNInput toNNInput()
	{
		NNInput nnInput;

		nnInput.position = hitPosition;
		nnInput.geometryID = geometryID;
		nnInput.instanceID = instanceID;
		nnInput.triangleID = triangleID;
		nnInput.barys = barys;

		return nnInput;
	}
};