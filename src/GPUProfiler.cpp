#include "GPUProfiler.h"
#include "D3D12Helpers.h"
#include "utils.h"

void Profiler::Initialize(ID3D12Device5* device)
{

	// Create query heap
	{
		SAFE_RELEASE(mQueryHeap);

		D3D12_QUERY_HEAP_DESC heapDesc = { };
		heapDesc.Count = cMaxQueryCount * 2;
		heapDesc.NodeMask = 0;
		heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		HRESULT hr = device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&mQueryHeap));
		utils::validate(hr, L"Error: failed to create profiling query heap!");
	}

	// Create readback heap for query results
	{
		SAFE_RELEASE(mReadbackBuffer);

		d3d12Helpers::createBuffer(device, D3D12_HEAP_TYPE_READBACK, 0, cMaxQueryCount * 2 * sizeof(uint64_t), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &mReadbackBuffer, L"Profiler readback");
	}
}

void Profiler::Cleanup()
{
	SAFE_RELEASE(mQueryHeap);
	SAFE_RELEASE(mReadbackBuffer);
	mCmdList = nullptr;
	mQueryCount = 0;
	mLastGpuFrequency = 0;
	mCachedTimes.clear();
}

std::string Profiler::BeginFrame(ID3D12GraphicsCommandList4* cmdList, ID3D12CommandQueue* cmdQueue)
{

	mCmdList = cmdList;
	std::string result = "";

	// Get resolved timings from previous frame
	if (mQueryCount > 0 && mLastGpuFrequency > 0) {

		// Download queries results
		D3D12_RANGE range;
		range.Begin = SIZE_T(0);
		range.End = SIZE_T(mQueryCount * 2 * sizeof(uint64_t));
		void* mappedData = nullptr;
		HRESULT hr = mReadbackBuffer->Map(0, &range, &mappedData);
		if (FAILED(hr))
		{
			utils::validate(hr, L"Error: failed to map profiling results!");
			mQueryCount = 0;
			StartEvent(L"Total GPU frame");
			return result;
		}

		uint64_t* queryData = ((uint64_t*)mappedData);

		for (size_t i = 0; i < mQueryCount; i++)
		{
			uint64_t startTime = queryData[i * 2];
			uint64_t endTime = queryData[i * 2 + 1];

			uint64_t delta = endTime - startTime;
			double frequency = double(mLastGpuFrequency);
			float queryTimeMs = (delta / frequency) * 1000.0;

			float cachedTimeMs = queryTimeMs;
			auto cachedTimeItem = mCachedTimes.find(mQueryNames[i]);
			if (cachedTimeItem != mCachedTimes.end())
			{
				cachedTimeMs = cachedTimeItem->second;
			}

			const float alpha = 0.99f;
			cachedTimeMs = alpha * cachedTimeMs + (1.0f - alpha) * queryTimeMs;
			mCachedTimes[mQueryNames[i]] = cachedTimeMs;

			char temp[256];
			snprintf(temp, 256, "%-15ls: %8.3fms (%.2fms)\n", mQueryNames[i].c_str(), queryTimeMs, cachedTimeMs);
			result += temp;
		}

		D3D12_RANGE writtenRange = { 0, 0 };
		mReadbackBuffer->Unmap(0, &writtenRange);
	}

	mQueryCount = 0;

	HRESULT hr = cmdQueue->GetTimestampFrequency(&mLastGpuFrequency);
	if (FAILED(hr) || mLastGpuFrequency == 0)
	{
		utils::validate(FAILED(hr) ? hr : E_FAIL, L"Error: failed to get GPU timestamp frequency!");
		mLastGpuFrequency = 0;
	}

	StartEvent(L"Total GPU frame");
	return result;
}

void Profiler::EndFrame()
{
	// All scoped events must have ended before resolving the frame's timestamps.
	StopEvent(0);
	mCmdList->ResolveQueryData(mQueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, 0, mQueryCount * 2, mReadbackBuffer, 0);
}

unsigned int Profiler::StartEvent(std::wstring name)
{

	if (mQueryCount >= cMaxQueryCount) {
		utils::validate(E_FAIL, L"Only 'cMaxQueryCount' profiles are supported, this is one too many!");
		return -1;
	}

	unsigned int queryIndex = mQueryCount++;
	mQueryNames[queryIndex] = name;

	// Timestamp query only supports EndQuery method (so we call it during StartEvent as well as EndEvent)
	mCmdList->EndQuery(mQueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryIndex * 2);

	return queryIndex;
}

void Profiler::StopEvent(unsigned int queryIndex)
{
	if (queryIndex >= mQueryCount) return;

	mCmdList->EndQuery(mQueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryIndex * 2 + 1);
}

ProfileEvent::ProfileEvent(Profiler* profiler, std::wstring name) {
	mId = profiler->StartEvent(name);
	mProfiler = profiler;
}

ProfileEvent::~ProfileEvent() {
	mProfiler->StopEvent(mId);
}