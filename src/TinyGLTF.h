#pragma once

#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include "thirdparty/tinygltf/tiny_gltf.h"

namespace tinygltf_helpers
{
    std::string decodeUri(const std::string& uri);
}
