#pragma once

#include "common.h"
#include "D3D12ResourceUploader.h"

class RTPass
{
public:

	void Initialize(ID3D12Device5* device, ID3D12GraphicsCommandList4* cmdList, D3D12ResourceUploader& uploader, std::wstring raygenName, ID3D12RootSignature* rootSignature, IDxcBlob& shaderBlob, std::wstring debugName = L"");
	void Dispatch(ID3D12GraphicsCommandList4* cmdList, uint32_t width, uint32_t height);

private:

	void createDx12Objects(ID3D12Device5* device, ID3D12GraphicsCommandList4* cmdList, D3D12ResourceUploader& uploader, LPCWSTR raygenName, LPCWSTR missName, LPCWSTR closestHitName, LPCWSTR hitGroupName, ID3D12RootSignature* rootSignature, IDxcBlob& shaderBlob);

	std::wstring					mName = L"RT Pass";

	ID3D12StateObject* mRTPSO = nullptr;
	D3D12_DISPATCH_RAYS_DESC		mDispatchDesc = {};

};
