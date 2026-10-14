#include "RTPass.h"
#include "utils.h"
#include "D3D12Helpers.h"
#include "shaders/shared.h"

using namespace d3d12Helpers;

void RTPass::Initialize(ID3D12Device5* device, ID3D12GraphicsCommandList4* cmdList, D3D12ResourceUploader& uploader, std::wstring raygenName, ID3D12RootSignature* rootSignature, IDxcBlob& shaderBlob, std::wstring debugName)
{
	mName = debugName;

	// Setup PSO, SBT and dispatch descriptor
	SAFE_RELEASE(mRTPSO);
	createDx12Objects(device, cmdList, uploader, raygenName.c_str(), L"Miss", L"ClosestHit", L"HitGroup", rootSignature, shaderBlob);
}

void RTPass::Dispatch(ID3D12GraphicsCommandList4* cmdList, uint32_t width, uint32_t height)
{
	mDispatchDesc.Width = width;
	mDispatchDesc.Height = height;
	mDispatchDesc.Depth = 1;

	cmdList->SetPipelineState1(mRTPSO);
	cmdList->DispatchRays(&mDispatchDesc);
}

void RTPass::createDx12Objects(ID3D12Device5* device, ID3D12GraphicsCommandList4* cmdList, D3D12ResourceUploader& uploader, LPCWSTR raygenName, LPCWSTR missName, LPCWSTR closestHitName, LPCWSTR hitGroupName, ID3D12RootSignature* rootSignature, IDxcBlob& shaderBlob)
{
	const LPCWSTR anyHitName = L"AnyHit";
	const LPCWSTR shadowAnyHitName = L"ShadowAnyHit";
	const LPCWSTR shadowMissName = L"ShadowMiss";
	const LPCWSTR shadowClosestHitName = L"ShadowClosestHit";
	const LPCWSTR shadowHitGroupName = L"ShadowHitGroup";
	ID3D12StateObjectProperties* RTPSOInfo = nullptr;
	ID3D12Resource* shaderTable = nullptr;

	// Create PSO
	{
		// Raygen shader export
		D3D12_EXPORT_DESC raygenExportDesc = {};
		raygenExportDesc.Name = raygenName;
		raygenExportDesc.ExportToRename = raygenName;
		raygenExportDesc.Flags = D3D12_EXPORT_FLAG_NONE;

		// Miss shader export
		D3D12_EXPORT_DESC missExportDesc = {};
		missExportDesc.Name = missName;
		missExportDesc.ExportToRename = missName;
		missExportDesc.Flags = D3D12_EXPORT_FLAG_NONE;

		// Closest hit shader export
		D3D12_EXPORT_DESC closestHitExportDesc = {};
		closestHitExportDesc.Name = closestHitName;
		closestHitExportDesc.ExportToRename = closestHitName;
		closestHitExportDesc.Flags = D3D12_EXPORT_FLAG_NONE;

		// Shadow shader exports
		D3D12_EXPORT_DESC shadowMissExportDesc = {};
		shadowMissExportDesc.Name = shadowMissName;
		D3D12_EXPORT_DESC shadowClosestHitExportDesc = {};
		shadowClosestHitExportDesc.Name = shadowClosestHitName;

		D3D12_EXPORT_DESC anyHitExportDesc = {};
		anyHitExportDesc.Name = anyHitName;
		D3D12_EXPORT_DESC shadowAnyHitExportDesc = {};
		shadowAnyHitExportDesc.Name = shadowAnyHitName;

		// Add exports to a subobject
		D3D12_EXPORT_DESC exportDescs[] = { raygenExportDesc, missExportDesc, closestHitExportDesc, shadowMissExportDesc, shadowClosestHitExportDesc, anyHitExportDesc, shadowAnyHitExportDesc };

		D3D12_DXIL_LIBRARY_DESC	dxilExportsDesc = {};
		dxilExportsDesc.DXILLibrary.BytecodeLength = shaderBlob.GetBufferSize();
		dxilExportsDesc.DXILLibrary.pShaderBytecode = shaderBlob.GetBufferPointer();
		dxilExportsDesc.NumExports = _countof(exportDescs);
		dxilExportsDesc.pExports = exportDescs;

		D3D12_STATE_SUBOBJECT exportsSubobject = {};
		exportsSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
		exportsSubobject.pDesc = &dxilExportsDesc;

		// Hit group subobject
		D3D12_HIT_GROUP_DESC hitGroupDesc = {};
		hitGroupDesc.ClosestHitShaderImport = closestHitName;
		hitGroupDesc.AnyHitShaderImport = anyHitName;
		hitGroupDesc.HitGroupExport = hitGroupName;

		D3D12_STATE_SUBOBJECT hitGroup = {};
		hitGroup.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
		hitGroup.pDesc = &hitGroupDesc;

		// Shadow hit group
		D3D12_HIT_GROUP_DESC shadowHitGroupDesc = {};
		shadowHitGroupDesc.ClosestHitShaderImport = shadowClosestHitName;
		shadowHitGroupDesc.AnyHitShaderImport = shadowAnyHitName;
		shadowHitGroupDesc.HitGroupExport = shadowHitGroupName;

		D3D12_STATE_SUBOBJECT shadowHitGroup = {};
		shadowHitGroup.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
		shadowHitGroup.pDesc = &shadowHitGroupDesc;

		// Shader config subobject
		D3D12_RAYTRACING_SHADER_CONFIG shaderDesc = {};
		shaderDesc.MaxPayloadSizeInBytes = sizeof(RayPayload);
		shaderDesc.MaxAttributeSizeInBytes = D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES;

		D3D12_STATE_SUBOBJECT shaderConfigObject = {};
		shaderConfigObject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
		shaderConfigObject.pDesc = &shaderDesc;

		// List shader export names that use payload
		LPCWSTR shaderExports[] = { raygenName, missName, hitGroupName, shadowMissName, shadowHitGroupName };

		// Association between shaders and the payload
		D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION shaderPayloadAssociation = {};
		shaderPayloadAssociation.NumExports = _countof(shaderExports);
		shaderPayloadAssociation.pExports = shaderExports;

		D3D12_STATE_SUBOBJECT shaderPayloadAssociationObject = {};
		shaderPayloadAssociationObject.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
		shaderPayloadAssociationObject.pDesc = &shaderPayloadAssociation;

		// Subobject for root signature
		D3D12_STATE_SUBOBJECT rootSignatureSubobject;
		rootSignatureSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
		rootSignatureSubobject.pDesc = &rootSignature;

		// Subobject for the RT pipeline configuration
		D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
		pipelineConfig.MaxTraceRecursionDepth = 1;

		D3D12_STATE_SUBOBJECT pipelineConfigObject = {};
		pipelineConfigObject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
		pipelineConfigObject.pDesc = &pipelineConfig;

		// Make a list of all subobjects
		D3D12_STATE_SUBOBJECT subobjects[] = {
			exportsSubobject, hitGroup, shaderConfigObject, shaderPayloadAssociationObject,
			rootSignatureSubobject,	pipelineConfigObject, shadowHitGroup };

		// Link shader payload association subobject to shader config object
		((D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*)subobjects[3].pDesc)->pSubobjectToAssociate = &subobjects[2];

		// PSO Descriptor
		D3D12_STATE_OBJECT_DESC pipelineDesc = {};
		pipelineDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
		pipelineDesc.NumSubobjects = _countof(subobjects);
		pipelineDesc.pSubobjects = subobjects;

		// Create the PSO
		HRESULT hr = device->CreateStateObject(&pipelineDesc, IID_PPV_ARGS(&mRTPSO));
		utils::validate(hr, L"Raytracing PSO creation failed!");
		mRTPSO->SetName((mName + L" RT PSO").c_str());

		// Get the PSO properties
		hr = mRTPSO->QueryInterface(IID_PPV_ARGS(&RTPSOInfo));
		utils::validate(hr, L"Raytracing PSO failed to grab info object!");
	}

	// Setup the shader binding table
	const size_t shaderTableRecordSize = ALIGN(D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
	{
		const uint32_t shaderTableSize = ALIGN(D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT, shaderTableRecordSize * 5);

		// Create the shader table buffer
		createBuffer(device, D3D12_HEAP_TYPE_DEFAULT, 0, shaderTableSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &shaderTable);

		// Build shader table data on CPU
		std::vector<uint8_t> shaderTableData(shaderTableSize, 0);
		uint8_t* pData = shaderTableData.data();

		// Shader Record 0 - Miss program for standard rays
		memcpy(pData, RTPSOInfo->GetShaderIdentifier(missName), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

		// Shader Record 1 - Miss program for shadow rays
		pData += shaderTableRecordSize;
		memcpy(pData, RTPSOInfo->GetShaderIdentifier(shadowMissName), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

		// Shader Record 2 - Hit Group for standard rays
		pData += shaderTableRecordSize;
		memcpy(pData, RTPSOInfo->GetShaderIdentifier(hitGroupName), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

		// Shader Record 3 - Hit Group for shadow rays
		pData += shaderTableRecordSize;
		memcpy(pData, RTPSOInfo->GetShaderIdentifier(shadowHitGroupName), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

		// Shader Record 4 - Ray Generation program
		pData += shaderTableRecordSize;
		memcpy(pData, RTPSOInfo->GetShaderIdentifier(raygenName), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

		// Upload shader table to GPU
		uploader.Upload(shaderTableData.data(), shaderTable);
		transitionBarrier(cmdList, shaderTable, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	}

	// Setup the dispatch descriptor
	{
		const auto& shaderTableAddress = shaderTable->GetGPUVirtualAddress();

		mDispatchDesc.MissShaderTable.StartAddress = shaderTableAddress;
		mDispatchDesc.MissShaderTable.SizeInBytes = shaderTableRecordSize * 2;
		mDispatchDesc.MissShaderTable.StrideInBytes = shaderTableRecordSize;

		mDispatchDesc.HitGroupTable.StartAddress = shaderTableAddress + shaderTableRecordSize * 2;
		mDispatchDesc.HitGroupTable.SizeInBytes = shaderTableRecordSize * 2;
		mDispatchDesc.HitGroupTable.StrideInBytes = shaderTableRecordSize;

		mDispatchDesc.RayGenerationShaderRecord.StartAddress = shaderTableAddress + (shaderTableRecordSize * 4);
		mDispatchDesc.RayGenerationShaderRecord.SizeInBytes = shaderTableRecordSize;
	}
}
