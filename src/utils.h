#pragma once
#include "common.h"

namespace utils {

	void validate(HRESULT hr, LPCWSTR msg);
	void validate(HRESULT hr, LPCSTR msg);
	void printMessage(const std::string& message);

	// Helper to locate assets and shaders (either next to .exe or fallback inside sources for development)
	std::wstring resolveContentFolder(const std::wstring& folder, const char* fallback);

	std::wstring extractExtension(std::wstring filePath);
	bool hasExtension(const std::wstring& filePath, const std::wstring& extension);
	bool hasExtension(const std::wstring& filePath, const std::wstring& extensionA, const std::wstring& extensionB);
	std::wstring getPath(std::wstring filePath);
	std::wstring getExePath();
	std::wstring resolvePath(std::wstring path);
	std::string wstringToString(const std::wstring& wstr);
	std::wstring stringToWstring(const std::string& wstr);

	float getDpiScale(HWND window);

	uint32_t divRoundUp(uint32_t x, uint32_t div);

	bool isKeyPressed(PBYTE keyboardState, BYTE key);
	glm::vec2 getMousePosition(HWND hWnd, float screenWidth, float screenHeight);

	std::string vectorToString(const glm::vec3& v);

	void AABBUnion(glm::vec3& aabbMin, glm::vec3& aabbMax, const glm::vec3 p);
	void AABBUnionTransformed(glm::vec3& aabbMin, glm::vec3& aabbMax, const glm::vec3& boundsMin, const glm::vec3& boundsMax, const glm::mat4& transform);

	uint32_t mixIntegers(uint32_t a, uint32_t b, uint32_t c);
}
