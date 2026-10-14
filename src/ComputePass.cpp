#include "ComputePass.h"
#include "utils.h"
#include "D3D12Helpers.h"

using namespace d3d12Helpers;

void ComputePass::Initialize(ID3D12Device5* device, ID3D12RootSignature* rootSignature, IDxcBlob& shaderBlob, std::wstring debugName)
{
	mName = debugName;

	SAFE_RELEASE(mPSO);
	mPSO = createComputePSO(device, shaderBlob, rootSignature, mName + L" Compute PSO");
}

void ComputePass::Dispatch(ID3D12GraphicsCommandList4* cmdList, const uint32_t width, const uint32_t height, const uint32_t threadGroupSize)
{
	const uint32_t dispatchWidth = utils::divRoundUp(width, threadGroupSize);
	const uint32_t dispatchHeight = utils::divRoundUp(height, threadGroupSize);

	cmdList->SetPipelineState(mPSO);
	cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);
}

