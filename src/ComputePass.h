#pragma once

#include "common.h"
#include "D3D12ResourceUploader.h"

class ComputePass
{
public:

	void Initialize(ID3D12Device5* device, ID3D12RootSignature* rootSignature, IDxcBlob& shaderBlob, std::wstring debugName = L"");

	void Dispatch(ID3D12GraphicsCommandList4* cmdList, const uint32_t width, const uint32_t height, const uint32_t threadGroupSize);

private:

	std::wstring		 mName = L"Compute Pass";
	ID3D12PipelineState* mPSO = nullptr;

};
