#pragma once

#include "common.h"
#include "utils.h"

class DxcShaderCompiler
{
public:

	void Initialize();

	IDxcBlob* CompileShader(LPCWSTR filename, LPCWSTR entryPoint, LPCWSTR targetProfile, std::vector<LPCWSTR> compilerFlags = {});

	void Release();

private:

	IDxcCompiler3* dxcCompiler = nullptr;
	IDxcUtils* dxcUtils = nullptr;

	std::vector<IDxcResult*> compiledShaders;
};
