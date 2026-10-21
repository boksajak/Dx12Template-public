#include "common.h"

// Library implementation
#define TINYGLTF_IMPLEMENTATION
#include "TinyGLTF.h"

std::string tinygltf_helpers::decodeUri(const std::string& uri)
{
    return tinygltf::dlib::urldecode(uri);
}
