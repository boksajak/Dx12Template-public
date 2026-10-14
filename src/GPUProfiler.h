#pragma once

#include "common.h"
#include "utils.h"

class Profiler
{
public:

	void Initialize(ID3D12Device5* device);

	void Cleanup();

	std::string BeginFrame(ID3D12GraphicsCommandList4* cmdList, ID3D12CommandQueue* cmdQueue);

	void EndFrame();

	unsigned int StartEvent(std::wstring name);

	void StopEvent(unsigned int queryIndex);

private:
	ID3D12GraphicsCommandList4* mCmdList = nullptr;
	ID3D12QueryHeap* mQueryHeap = nullptr;
	ID3D12Resource* mReadbackBuffer = nullptr;

	unsigned int mQueryCount = 0;
	static const unsigned int cMaxQueryCount = 33; // 32 scoped events plus the total GPU frame.
	std::wstring mQueryNames[cMaxQueryCount];
	uint64_t mLastGpuFrequency = 0;

	std::map<std::wstring, float> mCachedTimes;
};

class ProfileEvent
{
public:

	ProfileEvent(Profiler* profiler, std::wstring name);

	~ProfileEvent();

private:

	unsigned int mId;
	Profiler* mProfiler;
};

#define TOKENPASTE(x, y) x ## y
#define TOKENPASTE2(x, y) TOKENPASTE(x, y)
#define PROFILE(_name) ProfileEvent TOKENPASTE2(profilingContext, __COUNTER__)(&mProfiler[mCurrentFrameIndex], _name);
