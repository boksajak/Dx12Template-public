#include "utils.h"

#include <locale>
#include <codecvt>
#include <cwctype>
#include <filesystem>

void utils::validate(HRESULT hr, LPCWSTR msg)
{
	if (FAILED(hr))
	{
		MessageBox(NULL, msg, L"Error", MB_OK);
		PostQuitMessage(EXIT_FAILURE);
	}
}

void utils::validate(HRESULT hr, LPCSTR msg)
{
	if (FAILED(hr))
	{
		MessageBoxA(NULL, msg, "Error", MB_OK);
		PostQuitMessage(EXIT_FAILURE);
	}
}

void utils::printMessage(const std::string& message)
{
	OutputDebugStringA(message.c_str());
}

std::wstring utils::resolveContentFolder(const std::wstring& folder, const char* fallback)
{
	std::error_code err;

	// First try next to .exe
	const std::filesystem::path path = std::filesystem::path(getExePath()) / folder;
	if (std::filesystem::is_directory(path, err)) return path.wstring() + L"\\";

	// Go to fallback in the source directory for development
	if (fallback != nullptr && *fallback != '\0')
	{
		std::filesystem::path fallbackPath(fallback);
		err.clear();
		if (std::filesystem::is_directory(fallbackPath, err)) return fallbackPath.make_preferred().wstring() + L"\\";
	}

	// No folder exists, return the one next to .exe
	return path.wstring() + L"\\";
}

std::wstring utils::extractExtension(std::wstring filePath)
{
	auto lastDot = wcsrchr(filePath.c_str(), '.');
	if (!lastDot) return L"";
	return std::wstring(lastDot + 1);
}

bool utils::hasExtension(const std::wstring& filePath, const std::wstring& extension)
{
	const std::wstring actual = extractExtension(filePath);
	if (actual.size() != extension.size()) return false;
	for (size_t i = 0; i < actual.size(); i++) {
		if (std::towlower(actual[i]) != std::towlower(extension[i])) return false;
	}
	return true;
}

bool utils::hasExtension(const std::wstring& filePath, const std::wstring& extensionA, const std::wstring& extensionB)
{
	return hasExtension(filePath, extensionA) || hasExtension(filePath, extensionB);
}

std::wstring utils::getPath(std::wstring filePath) {

	auto lastSlash = wcsrchr(filePath.c_str(), '\\');
	auto lastForwardSlash = wcsrchr(filePath.c_str(), '/');

	if (lastForwardSlash && (!lastSlash || lastSlash < lastForwardSlash))
		lastSlash = lastForwardSlash;

	if (!lastSlash) return L".\\";

	return std::wstring(filePath.c_str(), lastSlash + 1);
}

std::wstring utils::getExePath() {

	TCHAR exePath[MAX_PATH];
	if (GetModuleFileName(NULL, exePath, MAX_PATH)) {
		return getPath(exePath);
	}

	return L".\\";
}

std::string utils::wstringToString(const std::wstring& wstr)
{
	static std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> converter;
	return converter.to_bytes(wstr);
}

std::wstring utils::stringToWstring(const std::string& wstr)
{
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
	return converter.from_bytes(wstr);
}

float utils::getDpiScale(HWND window) {

	unsigned int dpi = GetDpiForWindow(window);

	const float defaultDpi = 96.0f; //< Default monitor DPI of the yesteryear
	float dpiScale = dpi / defaultDpi;

	return dpiScale;
}

uint32_t utils::divRoundUp(uint32_t x, uint32_t div)
{
	return (x + div - 1) / div;
}


bool utils::isKeyPressed(PBYTE keyboardState, BYTE key)
{
	//return (GetAsyncKeyState(key) & 0x8000);
	return (keyboardState[key] & 0x80);
}

glm::vec2 utils::getMousePosition(HWND hWnd, float screenWidth, float screenHeight) {
	POINT cursorPos;
	GetCursorPos(&cursorPos);
	ScreenToClient(hWnd, &cursorPos);

	float nx = (float(cursorPos.x) * (1.0f / screenWidth * 2.0f) - 1.0f) * (screenWidth / screenHeight);
	float ny = -(float(cursorPos.y) * (1.0f / screenHeight * 2.0f) - 1.0f);

	return glm::vec2(nx, ny);
}

std::string utils::vectorToString(const glm::vec3& v)
{
	return std::to_string(v.x) + "; " + std::to_string(v.y) + "; " + std::to_string(v.z);
}

void utils::AABBUnion(glm::vec3& aabbMin, glm::vec3& aabbMax, const glm::vec3 p)
{
	aabbMin.x = glm::min(aabbMin.x, p.x);
	aabbMin.y = glm::min(aabbMin.y, p.y);
	aabbMin.z = glm::min(aabbMin.z, p.z);

	aabbMax.x = glm::max(aabbMax.x, p.x);
	aabbMax.y = glm::max(aabbMax.y, p.y);
	aabbMax.z = glm::max(aabbMax.z, p.z);
}

void utils::AABBUnionTransformed(glm::vec3& aabbMin, glm::vec3& aabbMax, const glm::vec3& boundsMin, const glm::vec3& boundsMax, const glm::mat4& transform)
{
	// Transform all 8 corners individually to account for translation, scale and rotation
	for (int z = 0; z < 2; z++)
	{
		for (int y = 0; y < 2; y++)
		{
			for (int x = 0; x < 2; x++)
			{
				const glm::vec3 localCorner(
					x == 0 ? boundsMin.x : boundsMax.x,
					y == 0 ? boundsMin.y : boundsMax.y,
					z == 0 ? boundsMin.z : boundsMax.z);

				const glm::vec3 transformedCorner = glm::vec3(transform * glm::vec4(localCorner, 1.0f));

				utils::AABBUnion(aabbMin, aabbMax, transformedCorner);
			}
		}
	}
}

// Resolves any input path to canonical absolute form
std::wstring utils::resolvePath(std::wstring path)
{
	std::replace(path.begin(), path.end(), L'/', L'\\');

	const DWORD requiredLength = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
	if (requiredLength > 0)
	{
		std::vector<wchar_t> fullPath(requiredLength);
		const DWORD writtenLength = GetFullPathNameW(path.c_str(), requiredLength, fullPath.data(), nullptr);
		if (writtenLength > 0 && writtenLength < requiredLength)
		{
			path.assign(fullPath.data(), writtenLength);
		}
	}

	return path;
}

// Hash three integers together, based on Bob Jenkins' final function from lookup3 file
// Source: https://www.burtleburtle.net/bob/c/lookup3.c
uint32_t utils::mixIntegers(uint32_t a, uint32_t b, uint32_t c)
{
	c += 3152589u; // Strat

	auto rot = [](const uint32_t x, const uint32_t k) {
		return ((x << k) | (x >> (32 - k)));
	};

	c ^= b; c -= rot(b, 14);
	a ^= c; a -= rot(c, 11);
	b ^= a; b -= rot(a, 25);
	c ^= b; c -= rot(b, 16);
	a ^= c; a -= rot(c, 4);
	b ^= a; b -= rot(a, 14);
	c ^= b; c -= rot(b, 24);

	return c;
}