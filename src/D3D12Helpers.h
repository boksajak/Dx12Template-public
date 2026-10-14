#pragma once

#include "common.h"

static const D3D12_HEAP_PROPERTIES UploadHeapProperties =
{
	D3D12_HEAP_TYPE_UPLOAD,
	D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
	D3D12_MEMORY_POOL_UNKNOWN,
	0, 0
};

static const D3D12_HEAP_PROPERTIES DefaultHeapProperties =
{
	D3D12_HEAP_TYPE_DEFAULT,
	D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
	D3D12_MEMORY_POOL_UNKNOWN,
	0, 0
};

static const D3D12_HEAP_PROPERTIES ReadbackHeapProperties =
{
	D3D12_HEAP_TYPE_READBACK,
	D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
	D3D12_MEMORY_POOL_UNKNOWN,
	0, 0
};

namespace d3d12Helpers
{
	struct D3D12Capabilities
	{
		D3D12_RAYTRACING_TIER raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
		bool shaderModel65 = false;
		bool supportsTearing = false; //< Queried from the DXGI factory

		bool supportsRayTracing() const { return raytracingTier >= D3D12_RAYTRACING_TIER_1_0; }
		bool supportsInlineRayTracing() const { return raytracingTier >= D3D12_RAYTRACING_TIER_1_1 && shaderModel65; }
	};

	D3D12Capabilities getDeviceCapabilities(ID3D12Device* device);
	bool supportsTearing(IDXGIFactory5* factory);

	UINT64 getResourceAllocationSize(ID3D12Device* device, ID3D12Resource* resource);

	// Loading budgets
	static constexpr UINT64 kUploadStagingBufferBudget = 32 * 1024 * 1024;
	static constexpr UINT64 kBuildScratchBudget = 32 * 1024 * 1024;
	static constexpr UINT64 kTemporaryBLASBudget = 32 * 1024 * 1024;
	static_assert(kUploadStagingBufferBudget >= 64 * 1024, "Upload staging buffer budget must be at least 64 KiB.");

	HRESULT createDescriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, D3D12_DESCRIPTOR_HEAP_FLAGS flags, ID3D12DescriptorHeap** ppHeap);

	void createBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heapType, UINT64 alignment, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ID3D12Resource** ppResource, std::wstring debugName = L"");

	// Single-mip, single-sample 2D texture in the default heap.
	void createTexture2D(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ID3D12Resource** ppResource, std::wstring debugName = L"");

	// The buffer must cover size rounded up to D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT.
	void createConstantBufferView(ID3D12Device* device, ID3D12Resource* resource, UINT size, D3D12_CPU_DESCRIPTOR_HANDLE handle);

	void createAccelerationStructureSRV(ID3D12Device* device, ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle);

	void createTexture2DSRV(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle);

	void createTexture2DUAV(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle);

	void createStructuredBufferSRV(ID3D12Device* device, ID3D12Resource* resource, UINT elementCount, UINT stride, D3D12_CPU_DESCRIPTOR_HANDLE handle);

	ID3D12PipelineState* createComputePSO(ID3D12Device* device, IDxcBlob& shaderBlob, ID3D12RootSignature* rootSignature, std::wstring debugName = L"");

	void transitionBarrier(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);

	void uavBarrier(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* resource);

};
