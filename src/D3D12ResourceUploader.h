#pragma once
#include "common.h"


class D3D12ResourceUploader
{
public:

	void Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, UINT64 capacity = 64 * 1024);
	void Upload(const void* inputData, ID3D12Resource* targetBuffer);
	void UploadTexture(const void* inputData, ID3D12Resource* targetTexture);
	void Flush();
	void Finish();

private:

	void Submit();
	void Wait(UINT64 fenceValue);

	struct UploadStagingBuffer
	{
		ID3D12Resource* buffer = nullptr;
		ID3D12CommandAllocator* allocator = nullptr;
		ID3D12GraphicsCommandList* cmdList = nullptr;
		uint8_t* mappedData = nullptr;
		UINT64 fenceValue = 0;
	};

	UploadStagingBuffer& PrepareStagingBuffer();

	UINT64 mCapacity = 0;
	ID3D12Device* mDevice = nullptr;

	// We ping-poing between two upload buffers to get overlap in uploads
	UploadStagingBuffer mStagingBuffers[2];
	UINT mCurrentStagingBuffer = 0;
	UINT64 mUsedBytes = 0;

	ID3D12CommandQueue* mQueue = nullptr;
	ID3D12Fence* mFence = nullptr;
	HANDLE mFenceEvent = nullptr;
	UINT64 mFenceValue = 0;
};
