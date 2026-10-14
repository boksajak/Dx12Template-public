#include "D3D12Helpers.h"
#include "utils.h"

d3d12Helpers::D3D12Capabilities d3d12Helpers::getDeviceCapabilities(ID3D12Device* device)
{
	D3D12Capabilities capabilities;
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 options = {};
	if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)))) capabilities.raytracingTier = options.RaytracingTier;

	// Query the shader model targeted by our shaders; older runtimes may reject it.
	D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = { D3D_SHADER_MODEL_6_5 };
	capabilities.shaderModel65 = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) && shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
	return capabilities;
}

bool d3d12Helpers::supportsTearing(IDXGIFactory5* factory)
{
	BOOL allowTearing = FALSE;
	return SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))) && allowTearing;
}

UINT64 d3d12Helpers::getResourceAllocationSize(ID3D12Device* device, ID3D12Resource* resource)
{
	if (!resource) return 0;
	const auto desc = resource->GetDesc();
	return device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
}

HRESULT d3d12Helpers::createDescriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, D3D12_DESCRIPTOR_HEAP_FLAGS flags, ID3D12DescriptorHeap** ppHeap)
{
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	desc.Type = type;
	desc.NumDescriptors = count;
	desc.Flags = flags;

	HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(ppHeap));
	utils::validate(hr, L"Error: failed to create descriptor heap!");
	return hr;
}

void d3d12Helpers::createBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heapType, UINT64 alignment, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ID3D12Resource** ppResource, std::wstring debugName)
{
	HRESULT hr;

	D3D12_HEAP_PROPERTIES heapDesc = {};
	heapDesc.Type = heapType;
	heapDesc.CreationNodeMask = 1;
	heapDesc.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC resourceDesc = {};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resourceDesc.Alignment = alignment;
	resourceDesc.Height = 1;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.MipLevels = 1;
	resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.SampleDesc.Quality = 0;
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resourceDesc.Width = size;
	resourceDesc.Flags = flags;

	// Create the GPU resource
	hr = device->CreateCommittedResource(&heapDesc, D3D12_HEAP_FLAG_NONE, &resourceDesc, state, nullptr, IID_PPV_ARGS(ppResource));
	utils::validate(hr, L"Error: failed to create buffer resource!");

	(*ppResource)->SetName(debugName.c_str());
}

void d3d12Helpers::createTexture2D(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ID3D12Resource** ppResource, std::wstring debugName)
{
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = flags;

	HRESULT hr = device->CreateCommittedResource(&DefaultHeapProperties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(ppResource));
	utils::validate(hr, L"Error: failed to create 2D texture!");
	(*ppResource)->SetName(debugName.c_str());
}

void d3d12Helpers::createConstantBufferView(ID3D12Device* device, ID3D12Resource* resource, UINT size, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {};
	desc.SizeInBytes = ALIGN(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, size);
	desc.BufferLocation = resource->GetGPUVirtualAddress();
	device->CreateConstantBufferView(&desc, handle);
}

void d3d12Helpers::createAccelerationStructureSRV(ID3D12Device* device, ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.RaytracingAccelerationStructure.Location = resource ? resource->GetGPUVirtualAddress() : 0;
	device->CreateShaderResourceView(nullptr, &desc, handle);
}

void d3d12Helpers::createTexture2DSRV(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.Format = format;
	desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView(resource, &desc, handle);
}

void d3d12Helpers::createTexture2DUAV(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
	desc.Format = format;
	desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	device->CreateUnorderedAccessView(resource, nullptr, &desc, handle);
}

void d3d12Helpers::createStructuredBufferSRV(ID3D12Device* device, ID3D12Resource* resource, UINT elementCount, UINT stride, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.Buffer.NumElements = elementCount;
	desc.Buffer.StructureByteStride = stride;
	device->CreateShaderResourceView(resource, &desc, handle);
}

ID3D12PipelineState* d3d12Helpers::createComputePSO(ID3D12Device* device, IDxcBlob& shaderBlob, ID3D12RootSignature* rootSignature, std::wstring debugName)
{
	D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc = {};
	pipelineDesc.pRootSignature = rootSignature;
	pipelineDesc.CS.pShaderBytecode = shaderBlob.GetBufferPointer();
	pipelineDesc.CS.BytecodeLength = shaderBlob.GetBufferSize();
	pipelineDesc.NodeMask = 0;
	pipelineDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

	ID3D12PipelineState* pso = nullptr;
	HRESULT hr = device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pso));
	utils::validate(hr, L"Error: failed to create compute PSO!");

	pso->SetName(debugName.c_str());

	return pso;
}

void d3d12Helpers::transitionBarrier(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = from;
	barrier.Transition.StateAfter = to;

	cmdList->ResourceBarrier(1, &barrier);
}

void d3d12Helpers::uavBarrier(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* resource)
{
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.UAV.pResource = resource;

	cmdList->ResourceBarrier(1, &barrier);
}
