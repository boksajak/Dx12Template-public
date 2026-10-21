#include "common.h"
#include <memory>
#include <cfloat>
#include "shaders/shared.h"
#include "shaders/brdf.h"


class Scene
{
public:

    // Load scene data from files
    HRESULT LoadOBJ(std::wstring filePath);
    HRESULT LoadGLTF(std::wstring filePath);

    // Extract triangular lights from loaded and transformed scene data
    std::vector<TriangleLight> ExtractLights() const;

    // Clear the scene structure
    void Clear();

    struct Image
    {
        int width = 0;
        int height = 0;

        struct TexelDataDeleter
        {
            void operator()(unsigned char* texels) const noexcept;
        };
        std::unique_ptr<unsigned char, TexelDataDeleter> texels;

        const unsigned char* data() const 
        { 
            return texels.get();
        }
        
        size_t size() const 
        { 
            return texels ? (size_t(width) * size_t(height) * 4) : 0;
        }
    };

    // A view to an image
    struct Texture
    {
        int imageIndex = -1;
        bool isSrgb = false;
    };

    struct Camera
    {
        glm::vec3 position;
        glm::vec3 forward;
        glm::vec3 up;
        float fovY = 42.0f;
    };

    struct Geometry
    {
        glm::uint vertexOffset = 0;
        glm::uint vertexCount = 0;
        glm::uint indexOffset = 0;
        glm::uint indexCount = 0;
        bool isOpaque = true;
        glm::vec3 boundsMin = glm::vec3(FLT_MAX);
        glm::vec3 boundsMax = glm::vec3(-FLT_MAX);
    };

    struct Instance
    {
        glm::uint geometryIndex = 0;
        glm::uint materialIndex = 0;
        glm::mat4 transform = glm::mat4(1.0f);
    };

    // Scene data
    std::vector<Vertex> vertices;
    std::vector<glm::uint> indices;
    std::vector<Geometry> geometries;
    std::vector<Instance> instances;
    std::vector<Material> materials;
    std::vector<Image> images;
    std::vector<Texture> textures;
    std::vector<Camera> cameras;

    glm::vec3 sceneMin;
    glm::vec3 sceneMax;
};
