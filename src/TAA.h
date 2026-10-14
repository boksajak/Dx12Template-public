#pragma once

#include "common.h"
#include "DxcShaderCompiler.h"

class TemporalAA
{
public:

	void Compile(ID3D12Device5* device, DxcShaderCompiler& compiler, const std::wstring& shaderFile);

	void Prepare(ID3D12Device5* device, UINT width, UINT height, ID3D12Resource* currentColor, ID3D12Resource* motionBarycentrics);

	void Dispatch(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* currentColor);

	void ResetHistory();

	UINT64 GetMemoryUsage() const { return mMemoryUsage; }

	void ReleaseResources();

	void Cleanup();

private:

	enum class DescriptorHeapConstants
	{
		SRVStart = 0,
		CurrentColor = SRVStart,
		HistoryColor,
		MotionBarycentrics,
		ResolvedColor,
		SRVTotal,

		UAVStart = SRVTotal,
		ResolvedOutput = UAVStart,
		HistoryOutput,
		HDROutput,
		Total,
		UAVTotal = Total - UAVStart
	};

	D3D12_CPU_DESCRIPTOR_HANDLE getDescriptorHandle(UINT index)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE handle = mHeap->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += mCbvSrvUavDescSize * index;
		return handle;
	}

	UINT mCbvSrvUavDescSize = 0;
	ID3D12RootSignature* mRootSignature = nullptr;
	ID3D12DescriptorHeap* mHeap = nullptr;
	ID3D12PipelineState* mResolvePSO = nullptr;
	ID3D12PipelineState* mUpdatePSO = nullptr;
	ID3D12Resource* mHistory = nullptr;
	ID3D12Resource* mResolved = nullptr;

	UINT64 mMemoryUsage = 0;
	UINT mWidth = 0;
	UINT mHeight = 0;

	bool mResetHistory = true;
};
