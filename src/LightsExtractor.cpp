#include "LightsExtractor.h"
#include "scene.h"
#include "utils.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace
{
    // Each leaf covers 4x4 texels, allowing its occupancy to fit in a 16-bit mask
    constexpr uint32_t kOccupancyBlockSize = 4;

    struct OccupancyLevel
    {
        // Dimensions in hierarchy nodes, not texels
        uint32_t width = 0;
        uint32_t height = 0;

        // Row-major flags: a node is occupied if any texel it covers has nonzero RGB
        std::vector<uint8_t> occupied;
    };

    struct OccupancyMask
    {
        uint32_t textureWidth = 0;
        uint32_t textureHeight = 0;

        // One mask per leaf block, with texel bits ordered row by row
        std::vector<uint16_t> blockTexelMasks;

        // Finest block level first, single-node root last
        std::vector<OccupancyLevel> levels;
    };

    enum class TextureCoverage
    {
        Emissive, // Potential overlap with nonzero texture RGB
        Black,    // No overlap with nonzero texture RGB
        Unknown   // Coverage could not be determined; retain the triangle conservatively
    };

    // Build a hierarchy of nonzero RGB occupancy for a tightly packed RGBA8 image
    bool buildTextureOccupancyMask(const Scene::Image& image, OccupancyMask& mask)
    {
        // Clear previous data so validation failure leaves an empty mask
        mask = {};
        if (image.width <= 0 || image.height <= 0) return false;

        // Validate image size without overflowing the texel or byte counts
        const size_t width = size_t(image.width);
        const size_t height = size_t(image.height);
        if (height > std::numeric_limits<size_t>::max() / width) return false;

        const size_t texelCount = width * height;
        if (texelCount > std::numeric_limits<size_t>::max() / 4 || image.size() != texelCount * 4) return false;

        mask.textureWidth = uint32_t(image.width);
        mask.textureHeight = uint32_t(image.height);

        // Round up to include partial blocks at the image edges
        OccupancyLevel baseLevel;
        baseLevel.width = (mask.textureWidth + kOccupancyBlockSize - 1) / kOccupancyBlockSize;
        baseLevel.height = (mask.textureHeight + kOccupancyBlockSize - 1) / kOccupancyBlockSize;
        const size_t blockCount = size_t(baseLevel.width) * size_t(baseLevel.height);
        baseLevel.occupied.resize(blockCount, 0);
        mask.blockTexelMasks.resize(blockCount, 0);

        // Build detailed texel masks and coarse occupancy flags for each leaf block
        for (uint32_t blockY = 0; blockY < baseLevel.height; blockY++)
        {
            for (uint32_t blockX = 0; blockX < baseLevel.width; blockX++)
            {
                // Restrict partial edge blocks to valid image texels
                const uint32_t texelMinX = blockX * kOccupancyBlockSize;
                const uint32_t texelMinY = blockY * kOccupancyBlockSize;
                const uint32_t texelMaxX = std::min(texelMinX + kOccupancyBlockSize, mask.textureWidth);
                const uint32_t texelMaxY = std::min(texelMinY + kOccupancyBlockSize, mask.textureHeight);

                uint16_t texelMask = 0;
                for (uint32_t y = texelMinY; y < texelMaxY; y++)
                {
                    for (uint32_t x = texelMinX; x < texelMaxX; x++)
                    {
                        // Only exactly black RGB is empty; alpha is ignored for emission occupancy
                        const size_t texelOffset = (size_t(y) * mask.textureWidth + x) * 4;
                        if (image.data()[texelOffset + 0] == 0 && image.data()[texelOffset + 1] == 0 && image.data()[texelOffset + 2] == 0) continue;

                        // Map the texel's block-local coordinates to its occupancy bit
                        const uint32_t localX = x - texelMinX;
                        const uint32_t localY = y - texelMinY;
                        texelMask |= uint16_t(1u << (localY * kOccupancyBlockSize + localX));
                    }
                }

                const size_t blockIndex = size_t(blockY) * baseLevel.width + blockX;
                mask.blockTexelMasks[blockIndex] = texelMask;
                baseLevel.occupied[blockIndex] = texelMask != 0;
            }
        }

        // Reduce 2x2 groups of nodes until a single root summarizes the entire image
        mask.levels.push_back(std::move(baseLevel));
        while (mask.levels.back().width > 1 || mask.levels.back().height > 1)
        {
            const OccupancyLevel& previous = mask.levels.back();
            OccupancyLevel next;
            next.width = (previous.width + 1) / 2;
            next.height = (previous.height + 1) / 2;
            next.occupied.resize(size_t(next.width) * next.height, 0);

            for (uint32_t y = 0; y < next.height; y++)
            {
                for (uint32_t x = 0; x < next.width; x++)
                {
                    // A parent is occupied if any valid child is occupied.
                    // Stop searching once an occupied child is found.
                    uint8_t occupied = 0;
                    for (uint32_t childY = 0; childY < 2 && !occupied; childY++)
                    {
                        const uint32_t previousY = y * 2 + childY;
                        if (previousY >= previous.height) continue;

                        for (uint32_t childX = 0; childX < 2; childX++)
                        {
                            const uint32_t previousX = x * 2 + childX;
                            if (previousX >= previous.width) continue;

                            if (previous.occupied[size_t(previousY) * previous.width + previousX])
                            {
                                occupied = 1;
                                break;
                            }
                        }
                    }
                    next.occupied[size_t(y) * next.width + x] = occupied;
                }
            }

            mask.levels.push_back(std::move(next));
        }

        return true;
    }

    // Test triangle-rectangle overlap using the separating axis theorem,
    // with conservative tolerances to retain borderline intersections
    bool triangleIntersectsRectangle(const glm::dvec2 triangle[3], double rectMinX, double rectMinY, double rectMaxX, double rectMaxY)
    {
        double triangleMinX = triangle[0].x;
        double triangleMaxX = triangle[0].x;
        double triangleMinY = triangle[0].y;
        double triangleMaxY = triangle[0].y;
        double coordinateMagnitude = 1.0;

        // Compute triangle bounds and track coordinate magnitude for a scale-dependent tolerance
        for (int i = 0; i < 3; i++)
        {
            triangleMinX = std::min(triangleMinX, triangle[i].x);
            triangleMaxX = std::max(triangleMaxX, triangle[i].x);
            triangleMinY = std::min(triangleMinY, triangle[i].y);
            triangleMaxY = std::max(triangleMaxY, triangle[i].y);
            coordinateMagnitude = std::max(coordinateMagnitude, std::abs(triangle[i].x));
            coordinateMagnitude = std::max(coordinateMagnitude, std::abs(triangle[i].y));
        }

        // Include rectangle coordinates when determining the tolerance
        coordinateMagnitude = std::max(coordinateMagnitude, std::abs(rectMinX));
        coordinateMagnitude = std::max(coordinateMagnitude, std::abs(rectMinY));
        coordinateMagnitude = std::max(coordinateMagnitude, std::abs(rectMaxX));
        coordinateMagnitude = std::max(coordinateMagnitude, std::abs(rectMaxY));

        // Borderline overlap retains the triangle. A false positive only keeps an extra light,
        // while a false negative would remove emitted energy.
        const double epsilon = 1e-9 * coordinateMagnitude;

        // Reject separation along the rectangle's X or Y axis
        if (triangleMaxX < rectMinX - epsilon || triangleMinX > rectMaxX + epsilon ||
            triangleMaxY < rectMinY - epsilon || triangleMinY > rectMaxY + epsilon)
        {
            return false;
        }

        // Center and half-size allow rectangle projection without testing all four corners
        const glm::dvec2 rectangleCenter(0.5 * (rectMinX + rectMaxX), 0.5 * (rectMinY + rectMaxY));
        const glm::dvec2 rectangleHalfSize(0.5 * (rectMaxX - rectMinX), 0.5 * (rectMaxY - rectMinY));

        // The remaining separating axes are perpendicular to the three triangle edges
        for (int edgeIndex = 0; edgeIndex < 3; edgeIndex++)
        {
            const glm::dvec2 edge = triangle[(edgeIndex + 1) % 3] - triangle[edgeIndex];
            const glm::dvec2 axis(-edge.y, edge.x);
            const double axisMagnitude = std::abs(axis.x) + std::abs(axis.y);

            // Skip near-zero edges that do not provide a useful separating axis
            if (axisMagnitude <= std::numeric_limits<double>::epsilon()) continue;

            // Project the triangle onto the axis to obtain its interval
            double triangleMinProjection = glm::dot(triangle[0], axis);
            double triangleMaxProjection = triangleMinProjection;
            for (int i = 1; i < 3; i++)
            {
                const double projection = glm::dot(triangle[i], axis);
                triangleMinProjection = std::min(triangleMinProjection, projection);
                triangleMaxProjection = std::max(triangleMaxProjection, projection);
            }

            // The rectangle's projected interval is center projection +/- projected radius
            const double rectangleCenterProjection = glm::dot(rectangleCenter, axis);
            const double rectangleRadius = rectangleHalfSize.x * std::abs(axis.x) + rectangleHalfSize.y * std::abs(axis.y);

            // Scale the tolerance to match the unnormalized projection axis
            const double projectionEpsilon = epsilon * axisMagnitude;

            // Disjoint projection intervals establish a separating axis
            if (triangleMaxProjection < rectangleCenterProjection - rectangleRadius - projectionEpsilon ||
                triangleMinProjection > rectangleCenterProjection + rectangleRadius + projectionEpsilon)
            {
                return false;
            }
        }

        // No separating axis was found
        return true;
    }

    // Test the triangle against the bilinear filtering support of each occupied texel in a leaf block
    bool triangleIntersectsOccupiedLeaf(const OccupancyMask& mask, uint32_t blockX, uint32_t blockY, const glm::dvec2 triangle[3], double textureOffsetX, double textureOffsetY)
    {
        const OccupancyLevel& baseLevel = mask.levels.front();
        if (blockX >= baseLevel.width || blockY >= baseLevel.height) return false;

        // Each bit marks an occupied texel within the block, ordered row by row
        const uint16_t texelMask = mask.blockTexelMasks[size_t(blockY) * baseLevel.width + blockX];
        const uint32_t blockMinX = blockX * kOccupancyBlockSize;
        const uint32_t blockMinY = blockY * kOccupancyBlockSize;

        for (uint32_t bitIndex = 0; bitIndex < kOccupancyBlockSize * kOccupancyBlockSize; bitIndex++)
        {
            // Skip texels that cannot emit
            if ((texelMask & uint16_t(1u << bitIndex)) == 0) continue;

            // Convert the bit index to image texel coordinates
            const uint32_t texelX = blockMinX + bitIndex % kOccupancyBlockSize;
            const uint32_t texelY = blockMinY + bitIndex / kOccupancyBlockSize;

            // Edge blocks may extend beyond the image dimensions
            if (texelX >= mask.textureWidth || texelY >= mask.textureHeight) continue;

            // For linear sampling, texel i can contribute while the scaled texture coordinate
            // lies in [i - 0.5, i + 1.5]. Test the full support rather than only the texel square.
            if (triangleIntersectsRectangle(triangle,
                textureOffsetX + texelX - 0.5, textureOffsetY + texelY - 0.5,
                textureOffsetX + texelX + 1.5, textureOffsetY + texelY + 1.5))
            {
                return true;
            }
        }

        return false;
    }

    // Check whether the UVs overlap any potentially emissive texel within a node
    bool triangleIntersectsOccupiedNode(const OccupancyMask& mask, size_t levelIndex, uint32_t nodeX, uint32_t nodeY, const glm::dvec2 triangle[3], double textureOffsetX, double textureOffsetY)
    {
        // Skip children outside the level dimensions and nodes containing no occupied texels
        const OccupancyLevel& level = mask.levels[levelIndex];
        if (nodeX >= level.width || nodeY >= level.height) return false;
        if (!level.occupied[size_t(nodeY) * level.width + nodeX]) return false;

        // Each hierarchy level doubles the node's texel span along both axes
        const uint64_t texelSpan = uint64_t(kOccupancyBlockSize) << levelIndex;

        // Compute node bounds in the current texture repetition, clipped to the image edges
        // Expand by half a texel to conservatively account for bilinear filtering
        const double nodeMinX = textureOffsetX + double(uint64_t(nodeX) * texelSpan) - 0.5;
        const double nodeMinY = textureOffsetY + double(uint64_t(nodeY) * texelSpan) - 0.5;
        const double nodeMaxX = textureOffsetX + double(std::min<uint64_t>(mask.textureWidth, uint64_t(nodeX + 1) * texelSpan)) + 0.5;
        const double nodeMaxY = textureOffsetY + double(std::min<uint64_t>(mask.textureHeight, uint64_t(nodeY + 1) * texelSpan)) + 0.5;
        
        // Reject the entire subtree when the triangle does not intersect the node bounds
        if (!triangleIntersectsRectangle(triangle, nodeMinX, nodeMinY, nodeMaxX, nodeMaxY)) return false;

        // At the finest level, test individual occupied texels within the block
        if (levelIndex == 0) return triangleIntersectsOccupiedLeaf(mask, nodeX, nodeY, triangle, textureOffsetX, textureOffsetY);

        // Descend into the four children and stop at the first occupied texel overlap
        for (uint32_t childY = 0; childY < 2; childY++)
        {
            for (uint32_t childX = 0; childX < 2; childX++)
            {
                if (triangleIntersectsOccupiedNode(mask, levelIndex - 1, nodeX * 2 + childX, nodeY * 2 + childY, triangle, textureOffsetX, textureOffsetY)) return true;
            }
        }

        return false;
    }

    // Check whether triangle overlaps emissive texels according to occupancy mask
    TextureCoverage classifyTextureCoverage(const TriangleLight& light, const OccupancyMask& mask)
    {
        // Require a single-cell root level and detailed texel masks for coverage testing
        if (mask.levels.empty() || mask.levels.back().width != 1 || mask.levels.back().height != 1 ||
            mask.levels.back().occupied.size() != 1 || mask.blockTexelMasks.empty())
        {
            return TextureCoverage::Unknown;
        }

        // Convert the triangle's UVs to texel coordinates, preserving values outside [0, 1] for texture repetition
        glm::dvec2 triangle[3];
        for (int i = 0; i < 3; i++)
        {
            if (!std::isfinite(light.vertexUV[i].x) || !std::isfinite(light.vertexUV[i].y)) return TextureCoverage::Unknown;
            triangle[i] = glm::dvec2(double(light.vertexUV[i].x) * mask.textureWidth, double(light.vertexUV[i].y) * mask.textureHeight);
        }

        // The root summarizes the entire image; no occupied texels means the texture is entirely black
        if (!mask.levels.back().occupied[0]) return TextureCoverage::Black;

        // Compute the triangle's axis-aligned bounds in texel space
        double minX = triangle[0].x;
        double maxX = triangle[0].x;
        double minY = triangle[0].y;
        double maxY = triangle[0].y;
        for (int i = 1; i < 3; i++)
        {
            minX = std::min(minX, triangle[i].x);
            maxX = std::max(maxX, triangle[i].x);
            minY = std::min(minY, triangle[i].y);
            maxY = std::max(maxY, triangle[i].y);
        }

        // Find the range of repeated texture tiles to test.
        // Expand the bounds by half a texel to include bilinear filtering across tile boundaries.
        const double textureWidth = mask.textureWidth;
        const double textureHeight = mask.textureHeight;
        const double tileMinXValue = std::floor((minX - 0.5) / textureWidth);
        const double tileMaxXValue = std::floor((maxX + 0.5) / textureWidth);
        const double tileMinYValue = std::floor((minY - 0.5) / textureHeight);
        const double tileMaxYValue = std::floor((maxY + 0.5) / textureHeight);

        // Fall back conservatively for extreme tile coordinates before converting to integers
        constexpr double kMaxSafeTileIndex = 1048576.0;
        if (tileMinXValue < -kMaxSafeTileIndex || tileMaxXValue > kMaxSafeTileIndex ||
            tileMinYValue < -kMaxSafeTileIndex || tileMaxYValue > kMaxSafeTileIndex)
        {
            return TextureCoverage::Unknown;
        }

        const int64_t tileMinX = int64_t(tileMinXValue);
        const int64_t tileMaxX = int64_t(tileMaxXValue);
        const int64_t tileMinY = int64_t(tileMinYValue);
        const int64_t tileMaxY = int64_t(tileMaxYValue);
        const uint64_t tileCountX = uint64_t(tileMaxX - tileMinX + 1);
        const uint64_t tileCountY = uint64_t(tileMaxY - tileMinY + 1);

        // Limit coverage testing to 64 tiles in total to bound the cost of heavily repeated UVs
        constexpr uint64_t kMaxTilesToTest = 64;
        if (tileCountX == 0 || tileCountY == 0 || tileCountX > kMaxTilesToTest || tileCountY > kMaxTilesToTest ||
            tileCountX * tileCountY > kMaxTilesToTest)
        {
            return TextureCoverage::Unknown;
        }

        // Traverse the occupancy hierarchy for each tile, offsetting it to that texture repetition
        const size_t rootLevel = mask.levels.size() - 1;
        for (int64_t tileY = tileMinY; tileY <= tileMaxY; tileY++)
        {
            for (int64_t tileX = tileMinX; tileX <= tileMaxX; tileX++)
            {
                if (triangleIntersectsOccupiedNode(mask, rootLevel, 0, 0, triangle, double(tileX) * textureWidth, double(tileY) * textureHeight))
                {
                    // Potentially emissive
                    return TextureCoverage::Emissive;
                }
            }
        }

        // No emissive texels were found under the triangle
        return TextureCoverage::Black;
    }
}

std::vector<TriangleLight> LightsExtractor::Extract(const Scene& scene)
{
    std::vector<TriangleLight> lights;

    // Find images referenced by emissive materials
    std::vector<uint8_t> emissiveImageUsed(scene.images.size(), 0);
    for (const Scene::Instance& instance : scene.instances)
    {
        const Material& material = scene.materials[instance.materialIndex];
        if (material.properties.emissive.x <= 0.0f && material.properties.emissive.y <= 0.0f && material.properties.emissive.z <= 0.0f) continue;
        if (material.emissiveTexture >= 0 && size_t(material.emissiveTexture) < scene.textures.size())
        {
            const int imageIndex = scene.textures[material.emissiveTexture].imageIndex;
            if (imageIndex >= 0 && size_t(imageIndex) < scene.images.size()) emissiveImageUsed[imageIndex] = 1;
        }
    }

    // Build one occupancy mask per referenced image, shared by all triangles using it
    std::vector<OccupancyMask> occupancyMasks(scene.images.size());
    for (size_t imageIndex = 0; imageIndex < scene.images.size(); imageIndex++)
    {
        if (emissiveImageUsed[imageIndex]) buildTextureOccupancyMask(scene.images[imageIndex], occupancyMasks[imageIndex]);
    }

    size_t candidateCount = 0;
    size_t texturedCandidateCount = 0;
    size_t culledCount = 0;
    size_t fallbackCount = 0;

    // Extract lights per instance because shared geometry can have different transforms and materials
    for (const Scene::Instance& instance : scene.instances)
    {
        const Material& material = scene.materials[instance.materialIndex];
        
        // Skip instances without emissive material
        if (material.properties.emissive.x <= 0.0f && material.properties.emissive.y <= 0.0f && material.properties.emissive.z <= 0.0f)
        {
            continue;
        }

        const Scene::Geometry& geometry = scene.geometries[instance.geometryIndex];
        for (glm::uint i = 0; i < geometry.indexCount; i += 3)
        {
            // Resolve vertex indices
            const Vertex& vertexA = scene.vertices[geometry.vertexOffset + scene.indices[geometry.indexOffset + i + 0]];
            const Vertex& vertexB = scene.vertices[geometry.vertexOffset + scene.indices[geometry.indexOffset + i + 1]];
            const Vertex& vertexC = scene.vertices[geometry.vertexOffset + scene.indices[geometry.indexOffset + i + 2]];

            TriangleLight light = {};

            // Transform positions according to this instance transformation
            light.vertexPosition[0] = glm::vec3(instance.transform * glm::vec4(vertexA.position, 1.0f));
            light.vertexPosition[1] = glm::vec3(instance.transform * glm::vec4(vertexB.position, 1.0f));
            light.vertexPosition[2] = glm::vec3(instance.transform * glm::vec4(vertexC.position, 1.0f));

            // Calculate normal and area
            const glm::vec3 edge1 = light.vertexPosition[1] - light.vertexPosition[0];
            const glm::vec3 edge2 = light.vertexPosition[2] - light.vertexPosition[0];
            const glm::vec3 unnormalizedNormal = glm::cross(edge2, edge1);
            const float doubleArea = glm::length(unnormalizedNormal);

            // Reject degenerate or non-finite triangles
            if (!(doubleArea > 0.0f) || !std::isfinite(doubleArea)) continue;

            // Preserve material emission and UVs for emission evaluation
            light.geometryNormal = unnormalizedNormal / doubleArea;
            light.surfaceArea = 0.5f * doubleArea;
            light.emissiveFactor = material.properties.emissive;
            light.vertexUV[0] = vertexA.uv;
            light.vertexUV[1] = vertexB.uv;
            light.vertexUV[2] = vertexC.uv;
            light.emissiveTextureIndex = material.emissiveTexture;

            candidateCount++;
            if (light.emissiveTextureIndex != INVALID_ID)
            {
                texturedCandidateCount++;

                // Test whether the triangle's UV footprint covers any potentially emissive texels
                TextureCoverage coverage = TextureCoverage::Unknown;
                if (light.emissiveTextureIndex >= 0 && size_t(light.emissiveTextureIndex) < scene.textures.size())
                {
                    const int imageIndex = scene.textures[light.emissiveTextureIndex].imageIndex;
                    if (imageIndex >= 0 && size_t(imageIndex) < occupancyMasks.size())
                    {
                        coverage = classifyTextureCoverage(light, occupancyMasks[imageIndex]);
                    }
                }

                // Cull only triangles classified as entirely black
                if (coverage == TextureCoverage::Black)
                {
                    culledCount++;
                    continue;
                }

                // Retain unknown coverage conservatively to avoid removing potential lights
                if (coverage == TextureCoverage::Unknown) fallbackCount++;
            }

            lights.push_back(light);
        }
    }

    if (candidateCount > 0)
    {
        // Print extraction report
        std::ostringstream message;
        message << "Triangle light extraction: " << candidateCount << " candidates (" << texturedCandidateCount << " textured), " << culledCount << " black textured triangles culled, " << lights.size() << " retained";
        
        if (fallbackCount > 0)
        {
            message << ", " << fallbackCount << " conservative fallbacks";
        }

        message << ".\n";
        utils::printMessage(message.str());
    }

    return lights;
}
