#include "D3D12ResourceUploader.h"
#include "D3D12Helpers.h"
#include "utils.h"

using namespace d3d12Helpers;

void D3D12ResourceUploader::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, UINT64 capacity)
{
	mDevice = device;
	mQueue = queue;
	mCapacity = ALIGN(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, std::max<UINT64>(64 * 1024, capacity));

	// Prepare fence for signalling upload completion
	utils::validate(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)), L"Failed to create upload fence!");
	mFenceEvent = CreateEventEx(nullptr, FALSE, FALSE, EVENT_ALL_ACCESS);
	if (!mFenceEvent) utils::validate(HRESULT_FROM_WIN32(GetLastError()), L"Failed to create upload fence event!");
}

// Upload newly created COPY_DEST buffers; call Finish before submitting their consuming commands.
// Between batches, buffers decay to COMMON and the next copy implicitly promotes them to COPY_DEST.
void D3D12ResourceUploader::Upload(const void* inputData, ID3D12Resource* targetBuffer)
{
	const UINT64 size = targetBuffer->GetDesc().Width;
	UINT64 targetOffset = 0;
	while (targetOffset < size)
	{
		if (mUsedBytes == mCapacity) Flush();
		auto& staging = PrepareStagingBuffer();
		const UINT64 copySize = std::min(size - targetOffset, mCapacity - mUsedBytes);
		memcpy(staging.mappedData + mUsedBytes, static_cast<const uint8_t*>(inputData) + targetOffset, static_cast<size_t>(copySize));
		staging.cmdList->CopyBufferRegion(targetBuffer, targetOffset, staging.buffer, mUsedBytes, copySize);
		mUsedBytes += copySize;
		targetOffset += copySize;
	}
}

// Upload the scene's tightly packed, single-mip RGBA8 images (including typeless resources).
void D3D12ResourceUploader::UploadTexture(const void* inputData, ID3D12Resource* targetTexture)
{
	const D3D12_RESOURCE_DESC desc = targetTexture->GetDesc();
	const UINT64 rowSize = desc.Width * 4;
	const UINT64 rowPitch = ALIGN(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, rowSize);

	UINT targetRow = 0;
	while (targetRow < desc.Height)
	{
		UINT64 offset = ALIGN(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, mUsedBytes);
		if (offset + rowPitch > mCapacity)
		{
			// Ping-poing to next staging buffer if this one is full
			Flush();
			offset = 0;
		}
		auto& staging = PrepareStagingBuffer();
		const UINT rowCount = static_cast<UINT>(std::min<UINT64>(desc.Height - targetRow, (mCapacity - offset) / rowPitch));
		for (UINT row = 0; row < rowCount; row++)
		{
			memcpy(staging.mappedData + offset + row * rowPitch, static_cast<const uint8_t*>(inputData) + (targetRow + row) * rowSize, static_cast<size_t>(rowSize));
		}

		D3D12_TEXTURE_COPY_LOCATION source = {};
		source.pResource = staging.buffer;
		source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		source.PlacedFootprint.Offset = offset;
		source.PlacedFootprint.Footprint.Format = desc.Format;
		source.PlacedFootprint.Footprint.Width = static_cast<UINT>(desc.Width);
		source.PlacedFootprint.Footprint.Height = rowCount;
		source.PlacedFootprint.Footprint.Depth = 1;
		source.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

		D3D12_TEXTURE_COPY_LOCATION target = {};
		target.pResource = targetTexture;
		target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		staging.cmdList->CopyTextureRegion(&target, 0, targetRow, 0, &source, nullptr);

		mUsedBytes = offset + rowCount * rowPitch;
		targetRow += rowCount;
	}
}

// Queue pending copies before submitting their consumers on the same graphics queue.
void D3D12ResourceUploader::Flush()
{
	if (mUsedBytes == 0) return;

	Submit();

	mCurrentStagingBuffer = (mCurrentStagingBuffer + 1) % 2;
	auto& staging = mStagingBuffers[mCurrentStagingBuffer];
	if (!staging.fenceValue) return; // A new staging buffer needs no wait or reset.

	// Fill the next staging buffer while the GPU consumes the submitted one, or wait before reusing a busy buffer.
	Wait(staging.fenceValue);

	utils::validate(staging.allocator->Reset(), L"Failed to reset upload command allocator!");
	utils::validate(staging.cmdList->Reset(staging.allocator, nullptr), L"Failed to reset upload command list!");
}

void D3D12ResourceUploader::Finish()
{
	if (!mFence) return;

	// Submit pending uploads and wait for finish
	Submit();
	Wait(mFenceValue);

	// Cleanup once uploads are finished
	for (auto& staging : mStagingBuffers)
	{
		if (staging.buffer) staging.buffer->Unmap(0, nullptr);
		SAFE_RELEASE(staging.cmdList);
		SAFE_RELEASE(staging.allocator);
		SAFE_RELEASE(staging.buffer);
		staging = {};
	}
	SAFE_RELEASE(mFence);
	CloseHandle(mFenceEvent);

	mFenceEvent = nullptr;
	mQueue = nullptr;
	mDevice = nullptr;
	mFenceValue = 0;
	mCurrentStagingBuffer = 0;
}

void D3D12ResourceUploader::Submit()
{
	if (mUsedBytes == 0) return;

	// Execute command list to upload this staging buffer
	auto& staging = mStagingBuffers[mCurrentStagingBuffer];
	utils::validate(staging.cmdList->Close(), L"Failed to close upload command list!");

	ID3D12CommandList* commandLists[] = { staging.cmdList };
	mQueue->ExecuteCommandLists(1, commandLists);

	// Signal completion on a fence
	staging.fenceValue = ++mFenceValue;
	utils::validate(mQueue->Signal(mFence, staging.fenceValue), L"Failed to signal upload fence!");
	mUsedBytes = 0;
}

void D3D12ResourceUploader::Wait(UINT64 fenceValue)
{
	if (mFence->GetCompletedValue() >= fenceValue) return;
	utils::validate(mFence->SetEventOnCompletion(fenceValue, mFenceEvent), L"Failed to set upload fence event!");
	WaitForSingleObjectEx(mFenceEvent, INFINITE, FALSE);
}

// Lazily prepare the current staging buffer, or return it if already allocated
D3D12ResourceUploader::UploadStagingBuffer& D3D12ResourceUploader::PrepareStagingBuffer()
{
	auto& staging = mStagingBuffers[mCurrentStagingBuffer];
	if (staging.buffer) return staging;

	// Allocate the current upload staging buffer
	createBuffer(mDevice, D3D12_HEAP_TYPE_UPLOAD, 0, mCapacity, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, &staging.buffer, L"Shared Resource Upload");

	// Map it to CPU pointer
	const D3D12_RANGE readRange = { 0, 0 };
	utils::validate(staging.buffer->Map(0, &readRange, reinterpret_cast<void**>(&staging.mappedData)), L"Failed to map upload buffer!");

	utils::validate(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&staging.allocator)), L"Failed to create upload command allocator!");
	utils::validate(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, staging.allocator, nullptr, IID_PPV_ARGS(&staging.cmdList)), L"Failed to create upload command list!");

	return staging;
}
