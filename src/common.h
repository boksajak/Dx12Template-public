#pragma once

#ifndef NOMINMAX
	#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

// Common includes
#include <unordered_map>
#include <map>
#include <chrono>
#include <fstream>
#include <sstream>
#include <algorithm>

// GLM library
#define GLM_FORCE_RADIANS
#define GLM_ENABLE_EXPERIMENTAL
#include "glm/glm.hpp"
#include "glm/gtc/constants.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtc/quaternion.hpp"

// DX12 includes
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

#include <dxgi1_6.h>
#include <d3d12.h>
#include <dxgidebug.h>

#include "d3dx12.h"

// DX Compiler includes
#include "dxcapi.h"

// Dx12 helpers
#define SAFE_RELEASE( x ) { if ( x ) { x->Release(); x = NULL; } }
#define ALIGN(_alignment, _val) (((_val + _alignment - 1) / _alignment) * _alignment)