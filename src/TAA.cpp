#include "TAA.h"
#include "D3D12Helpers.h"
#include "utils.h"

using namespace d3d12Helpers;


void TemporalAA::Compile(ID3D12Device5* device, DxcShaderCompiler& compiler, const std::wstring& shaderFile)
{
	if (!mRootSignature)
	{
		// SRVs
		D3D12_DESCRIPTOR_RANGE srvRange = {};
		srvRange.BaseShaderRegister = 0;
		srvRange.NumDescriptors = UINT(DescriptorHeapConstants::SRVTotal);
		srvRange.RegisterSpace = 0;
		srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		srvRange.OffsetInDescriptorsFromTableStart = UINT(DescriptorHeapConstants::SRVStart);

		// UAVs
		D3D12_DESCRIPTOR_RANGE uavRange = {};
		uavRange.BaseShaderRegister = 0;
		uavRange.NumDescriptors = UINT(DescriptorHeapConstants::UAVTotal);
		uavRange.RegisterSpace = 0;
		uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		uavRange.OffsetInDescriptorsFromTableStart = UINT(DescriptorHeapConstants::UAVStart);

		D3D12_DESCRIPTOR_RANGE ranges[] = { srvRange, uavRange };

		D3D12_ROOT_PARAMETER parameters[2] = {};
		parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		parameters[0].DescriptorTable.NumDescriptorRanges = _countof(ranges);
		parameters[0].DescriptorTable.pDescriptorRanges = ranges;
		parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		parameters[1].Constants.ShaderRegister = 0;
		parameters[1].Constants.RegisterSpace = 0;
		parameters[1].Constants.Num32BitValues = 3;

		D3D12_STATIC_SAMPLER_DESC sampler = {};
		sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.MaxAnisotropy = 1;
		sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		sampler.MinLOD = 0.0f;
		sampler.MaxLOD = D3D12_FLOAT32_MAX;
		sampler.ShaderRegister = 0;
		sampler.RegisterSpace = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
		rootDesc.NumParameters = _countof(parameters);
		rootDesc.pParameters = parameters;
		rootDesc.NumStaticSamplers = 1;
		rootDesc.pStaticSamplers = &sampler;
		rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		ID3DBlob* signature = nullptr;
		ID3DBlob* errors = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors);
		SAFE_RELEASE(errors);

		utils::validate(hr, L"Failed to serialize TAA root signature!");
		hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&mRootSignature));
		utils::validate(hr, L"Failed to create TAA root signature!");

		SAFE_RELEASE(signature);
	}

	SAFE_RELEASE(mResolvePSO);
	SAFE_RELEASE(mUpdatePSO);

	// Compile main TAA shader
	{
		IDxcBlob* shaderBlob = compiler.CompileShader(shaderFile.c_str(), L"MainTAA", L"cs_6_5");
		mResolvePSO = createComputePSO(device, *shaderBlob, mRootSignature, L"TAA resolve");
		SAFE_RELEASE(shaderBlob);
	}

	// Compile helper shader to update history
	{
		IDxcBlob* shaderBlob = compiler.CompileShader(shaderFile.c_str(), L"UpdateTAAHistory", L"cs_6_5");
		mUpdatePSO = createComputePSO(device, *shaderBlob, mRootSignature, L"TAA history and HDR output");
		SAFE_RELEASE(shaderBlob);
	}

	ResetHistory();
}

void TemporalAA::Prepare(ID3D12Device5* device, UINT width, UINT height, ID3D12Resource* currentColor, ID3D12Resource* motionBarycentrics)
{
	if (mHistory && mWidth == width && mHeight == height) return;
	ReleaseResources(); // Caller has completed the preceding frame before changing resources.

	mWidth = width;
	mHeight = height;

	// Create history and output buffers
	{
		createTexture2D(device, mWidth, mHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &mHistory, L"TAA history");
		createTexture2D(device, mWidth, mHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &mResolved, L"TAA resolved color");
	}

	mMemoryUsage = getResourceAllocationSize(device, mHistory) + getResourceAllocationSize(device, mResolved);

	// Create descriptor heap
	{
		createDescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, UINT(DescriptorHeapConstants::Total), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, &mHeap);

		mCbvSrvUavDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}

	// Create current color SRV
	{
		createTexture2DSRV(device, currentColor, DXGI_FORMAT_R32G32B32A32_FLOAT, getDescriptorHandle(UINT(DescriptorHeapConstants::CurrentColor)));
	}

	// Create history and resolved color SRVs
	{
		createTexture2DSRV(device, mHistory, DXGI_FORMAT_R16G16B16A16_FLOAT, getDescriptorHandle(UINT(DescriptorHeapConstants::HistoryColor)));
		createTexture2DSRV(device, mResolved, DXGI_FORMAT_R16G16B16A16_FLOAT, getDescriptorHandle(UINT(DescriptorHeapConstants::ResolvedColor)));
	}

	// Create motion-vector SRV (barycentrics share the texture's ZW channels).
	{
		createTexture2DSRV(device, motionBarycentrics, DXGI_FORMAT_R32G32B32A32_FLOAT, getDescriptorHandle(UINT(DescriptorHeapConstants::MotionBarycentrics)));
	}

	// Create history and resolved output UAVs
	{
		createTexture2DUAV(device, mResolved, DXGI_FORMAT_R16G16B16A16_FLOAT, getDescriptorHandle(UINT(DescriptorHeapConstants::ResolvedOutput)));
		createTexture2DUAV(device, mHistory, DXGI_FORMAT_R16G16B16A16_FLOAT, getDescriptorHandle(UINT(DescriptorHeapConstants::HistoryOutput)));
	}

	// Create HDR output UAV (reuse the current color buffer after resolving)
	{
		createTexture2DUAV(device, currentColor, DXGI_FORMAT_R32G32B32A32_FLOAT, getDescriptorHandle(UINT(DescriptorHeapConstants::HDROutput)));
	}
}

void TemporalAA::Dispatch(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* currentColor)
{
	// Transition resources to reading
	transitionBarrier(cmdList, currentColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	// Run the main pass
	{
		cmdList->SetDescriptorHeaps(1, &mHeap);
		cmdList->SetComputeRootSignature(mRootSignature);
		cmdList->SetComputeRootDescriptorTable(0, mHeap->GetGPUDescriptorHandleForHeapStart());

		// Setup root constants
		struct Constants { UINT width; UINT height; UINT reset; };
		const Constants constants = { mWidth, mHeight, mResetHistory ? 1u : 0u };
		static_assert(sizeof(Constants) == 3 * sizeof(UINT));
		cmdList->SetComputeRoot32BitConstants(1, 3, &constants, 0);

		cmdList->SetPipelineState(mResolvePSO);
		cmdList->Dispatch(utils::divRoundUp(mWidth, 16u), utils::divRoundUp(mHeight, 16u), 1);
	}

	// Dispatch the update history pass
	{
		transitionBarrier(cmdList, mResolved, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		transitionBarrier(cmdList, mHistory, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		transitionBarrier(cmdList, currentColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		cmdList->SetPipelineState(mUpdatePSO);
		cmdList->Dispatch(utils::divRoundUp(mWidth, 16u), utils::divRoundUp(mHeight, 16u), 1);
	}

	// Transition resources for next usage
	{
		transitionBarrier(cmdList, mHistory, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		transitionBarrier(cmdList, mResolved, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	mResetHistory = false;
}

void TemporalAA::ResetHistory()
{
	mResetHistory = true;
}

void TemporalAA::ReleaseResources()
{
	SAFE_RELEASE(mHistory);
	SAFE_RELEASE(mResolved);
	SAFE_RELEASE(mHeap);
	mWidth = mHeight = 0;
	mMemoryUsage = 0;
	ResetHistory();
}

void TemporalAA::Cleanup()
{
	ReleaseResources();
	SAFE_RELEASE(mResolvePSO);
	SAFE_RELEASE(mUpdatePSO);
	SAFE_RELEASE(mRootSignature);
}
