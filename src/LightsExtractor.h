#pragma once

#include "shaders/shared.h"
#include <vector>

class Scene;

namespace LightsExtractor
{
    // Extract emissive triangles from meshes as lights
    std::vector<TriangleLight> Extract(const Scene& scene);
}
