#include "DxcShaderCompiler.h"

void DxcShaderCompiler::Initialize()
{
	HRESULT hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxcCompiler));
	utils::validate(hr, L"Failed to create IDxcCompiler3 instance!");

	hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxcUtils));
	utils::validate(hr, L"Failed to create IDxcUtils instance!");
}

IDxcBlob* DxcShaderCompiler::CompileShader(LPCWSTR filename, LPCWSTR entryPoint, LPCWSTR targetProfile, std::vector<LPCWSTR> compilerFlags)
{
	IDxcResult* result = nullptr;
	HRESULT hr = S_OK;

	bool shaderCompiled = false;
	while (!shaderCompiled) {

		// Load the shader source file
		IDxcBlobEncoding* shaderSource = nullptr;
		hr = dxcUtils->LoadFile(filename, nullptr, &shaderSource);
		utils::validate(hr, L"Error: failed to create blob from shader file!");

		// Turn the shader source into a buffer object
		DxcBuffer sourceBuffer = {};
		sourceBuffer.Ptr = shaderSource->GetBufferPointer();
		sourceBuffer.Size = shaderSource->GetBufferSize();
		sourceBuffer.Encoding = DXC_CP_ACP;

		// Create the compiler include handler
		IDxcIncludeHandler* dxcIncludeHandler;
		hr = dxcUtils->CreateDefaultIncludeHandler(&dxcIncludeHandler);
		utils::validate(hr, L"Error: failed to create include handler");

		// Additional compiler flags (always present)
		compilerFlags.push_back(DXC_ARG_ALL_RESOURCES_BOUND);
		compilerFlags.push_back(DXC_ARG_WARNINGS_ARE_ERRORS); //< -WX
		compilerFlags.push_back(L"-enable-16bit-types");
		compilerFlags.push_back(L"-encoding utf8"); //< Force outputs to be in UTF-8
		compilerFlags.push_back(L"-Qstrip_reflect");

#if _DEBUG
		compilerFlags.push_back(DXC_ARG_DEBUG); //< -Zi
#else
		compilerFlags.push_back(L"-Qstrip_debug");
#endif

		// Build arguments for the compile command
		IDxcCompilerArgs* compilerArgs = nullptr;
		dxcUtils->BuildArguments(filename, entryPoint, targetProfile,
			compilerFlags.data(), (UINT32)compilerFlags.size(),
			nullptr, 0, &compilerArgs);

		// Compile the shader
		hr = dxcCompiler->Compile(&sourceBuffer, compilerArgs->GetArguments(), compilerArgs->GetCount(), dxcIncludeHandler, IID_PPV_ARGS(&result));
		utils::validate(hr, L"Error: failed to compile shader!");

		// Release the compile operation resources
		shaderSource->Release();
		dxcIncludeHandler->Release();
		compilerArgs->Release();

		// Verify the result 
		result->GetStatus(&hr);
		if (FAILED(hr))
		{
			// Read errors
			IDxcBlobUtf8* compileErrors;
			hr = result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&compileErrors), nullptr);
			utils::validate(hr, L"Error: failed to retrieve DXC compilation errors!");

			// Ask user if he wants to try recompiling again?
			bool retry = (MessageBox(nullptr, utils::stringToWstring(compileErrors->GetStringPointer()).c_str(), L"DXC Shader Compiler Error", MB_RETRYCANCEL) == IDRETRY);
			compileErrors->Release();

			if (retry)
			{
				continue;
			}
			else
			{
				return nullptr;
			}
		}

		// Successful compilation
		shaderCompiled = true;
	}

	IDxcBlob* compiledShaderBlob = nullptr;
	hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&compiledShaderBlob), nullptr);
	utils::validate(hr, L"Error: failed to get compiled shader blob!");

	compiledShaders.push_back(result);

	return compiledShaderBlob;
}

void DxcShaderCompiler::Release()
{
	SAFE_RELEASE(dxcCompiler);
	SAFE_RELEASE(dxcUtils);

	for (auto& shader : compiledShaders)
	{
		SAFE_RELEASE(shader);
	}

	compiledShaders.clear();

}
