#include "scene.h"
#include "LightsExtractor.h"
#include "utils.h"

#include "stb_image.h"
#include "tiny_obj_loader.h"
#include "TinyGLTF.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <tuple>
#include <unordered_map>
#include <algorithm>
#include <limits>
#include <atomic>
#include <future>
#include <thread>

void Scene::Image::TexelDataDeleter::operator()(unsigned char* texels) const noexcept
{
    stbi_image_free(texels);
}

namespace texture_helpers
{

    struct ImageKey
    {
        std::wstring imagePath;
        int buffer = -1;
        size_t byteOffset = 0;
        size_t byteLength = 0;
        const unsigned char* encodedData = nullptr;
        size_t encodedSize = 0;

        bool operator==(const ImageKey& other) const
        {
            return std::tie(imagePath, buffer, byteOffset, byteLength, encodedData, encodedSize)
                == std::tie(other.imagePath, other.buffer, other.byteOffset, other.byteLength, other.encodedData, other.encodedSize);
        }
    };

    struct ImageKeyHash
    {
        size_t operator()(const ImageKey& key) const
        {
            size_t hash = std::hash<std::wstring>{}(key.imagePath);
            
            auto combine = [&](size_t value) { hash ^= value + size_t(0x9e3779b9u) + (hash << 6) + (hash >> 2); };
            
            combine(std::hash<int>{}(key.buffer));
            combine(std::hash<size_t>{}(key.byteOffset));
            combine(std::hash<size_t>{}(key.byteLength));
            combine(std::hash<size_t>{}(key.encodedSize));
            combine(std::hash<const unsigned char*>{}(key.encodedData));

            return hash;
        }
    };

    // External images are skipped by TINYGLTF_NO_EXTERNAL_IMAGE. GLB bytes stay in model.buffers;
    // only data-URI payloads must be retained because TinyGLTF passes temporary compressed bytes.
    bool deferImageLoad(tinygltf::Image* image, int, std::string*, std::string*, int, int, const unsigned char* bytes, int size, void*)
    {
        if (image->bufferView < 0 && size > 0)
        {
            image->image.assign(bytes, bytes + size);
        }

        return true;
    }

    // Construct a key for comparing images. Handles images loaded from files or GLTF memory
    bool getImageKey(const std::wstring& sceneFile, const tinygltf::Model& model, int imageIndex, ImageKey& key)
    {
        const auto& image = model.images[imageIndex];

        if (image.bufferView >= 0)
        {
            if (size_t(image.bufferView) >= model.bufferViews.size()) return false;
            
            const auto& view = model.bufferViews[image.bufferView];
            if (view.buffer < 0 || size_t(view.buffer) >= model.buffers.size()) return false;
            
            const auto& buffer = model.buffers[view.buffer].data;
            if (!view.byteLength || view.byteOffset > buffer.size() || view.byteLength > buffer.size() - view.byteOffset) return false;
           
            key.buffer = view.buffer;
            key.byteOffset = view.byteOffset;
            key.byteLength = view.byteLength;
        }
        else if (!image.uri.empty())
        {
            const std::wstring name = utils::stringToWstring(tinygltf_helpers::decodeUri(image.uri));
            if (name.empty()) return false;
            
            const bool absolute = name[0] == L'/' || name[0] == L'\\' || (name.size() > 1 && name[1] == L':');

            // Resolve to absolute path. This makes sure that different notations leading to same file are resolved to same path for the texture key
            key.imagePath = utils::resolvePath(absolute ? name : utils::getPath(sceneFile) + name);
        }
        else if (!image.image.empty())
        {
            key.encodedData = image.image.data();
            key.encodedSize = image.image.size();
        }
        else
        {
            return false;
        }

        return true;
    }

    bool validImageSize(int width, int height)
    {
        return width > 0 && height > 0 && width <= 16384 && height <= 16384;
    }

    // Probe dimensions before allocating texels, then retain STB's RGBA8 allocation directly.
    bool loadImageFile(const std::wstring& path, Scene::Image& image)
    {
        FILE* file = _wfopen(path.c_str(), L"rb");
        if (!file) return false;
        
        // Prescan the image
        int channels = 0;
        const bool valid = stbi_info_from_file(file, &image.width, &image.height, &channels) && validImageSize(image.width, image.height);
        
        if (valid)
        {
            // Load image using STB lib. Require 4 bytes per pixel
            rewind(file);
            image.texels.reset(stbi_load_from_file(file, &image.width, &image.height, &channels, 4));
        }

        fclose(file);
        return image.texels != nullptr;
    }

    bool loadImageMemory(const unsigned char* bytes, size_t size, Scene::Image& image)
    {
        if (!size || size > size_t(std::numeric_limits<int>::max())) return false;
        int channels = 0;
        if (!stbi_info_from_memory(bytes, int(size), &image.width, &image.height, &channels) || !validImageSize(image.width, image.height)) return false;
        image.texels.reset(stbi_load_from_memory(bytes, int(size), &image.width, &image.height, &channels, 4));
        return image.texels != nullptr;
    }

    // Returns the GLTF texture index if the texture is supported, or -1 if it is not supported.
    int getSupportedTextureIndex(const tinygltf::TextureInfo& textureInfo)
    {
        return textureInfo.index >= 0 
            && textureInfo.texCoord == 0 //< We only load TEX_COORD_0 for now, ignore other texture coordinates
            && textureInfo.extensions.empty() ? textureInfo.index : -1;
    }

}

void Scene::Clear()
{
    // Clear data
    vertices.clear();
    indices.clear();
    geometries.clear();
    instances.clear();
    textures.clear();
    images.clear();
    cameras.clear();

    // Reset scene bounds
    sceneMin = glm::vec3(FLT_MAX, FLT_MAX, FLT_MAX);
    sceneMax = glm::vec3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    // Emplace one default material for geometry with no specified material
    materials.assign(1, Material{});
}

std::vector<TriangleLight> Scene::ExtractLights() const
{
    return LightsExtractor::Extract(*this);
}

namespace obj_helpers
{
    struct MaterialGroup
    {
        size_t indexOffset = 0;
        size_t indexCount = 0;
        size_t indicesWritten = 0;
        glm::vec3 boundsMin = glm::vec3(FLT_MAX);
        glm::vec3 boundsMax = glm::vec3(-FLT_MAX);
    };

    bool IsValidTriangle(const tinyobj::attrib_t& attrib, const tinyobj::mesh_t& mesh, size_t indexOffset)
    {
        for (size_t vertexIndex = 0; vertexIndex < 3; vertexIndex++)
        {
            const auto& source = mesh.indices[indexOffset + vertexIndex];
            if (source.vertex_index < 0 || size_t(source.vertex_index) >= attrib.vertices.size() / 3 ||
                source.normal_index < -1 || (source.normal_index >= 0 && size_t(source.normal_index) >= attrib.normals.size() / 3) ||
                source.texcoord_index < -1 || (source.texcoord_index >= 0 && size_t(source.texcoord_index) >= attrib.texcoords.size() / 2)) return false;
        }
        return true;
    }

    glm::uint GetMaterialIndex(const tinyobj::mesh_t& mesh, size_t faceIndex, size_t materialCount)
    {
        const int material = (faceIndex < mesh.material_ids.size()) ? mesh.material_ids[faceIndex] : -1;
        return (material >= 0 && size_t(material) < materialCount) ? (glm::uint(material) + 1) : 0;
    }

    // Count valid triangle indices per material and assign contiguous ranges in the final index buffer.
    size_t PrescanScene(const tinyobj::attrib_t& attrib, const std::vector<tinyobj::shape_t>& shapes, size_t materialCount, std::vector<MaterialGroup>& materialGroups)
    {
        materialGroups.assign(materialCount + 1, MaterialGroup{});
        for (const auto& shape : shapes)
        {
            const auto& mesh = shape.mesh;
            size_t indexOffset = 0;
            for (size_t f = 0; f < mesh.num_face_vertices.size(); f++)
            {
                const size_t count = size_t(mesh.num_face_vertices[f]);
                if (indexOffset > mesh.indices.size() || count > mesh.indices.size() - indexOffset) break;
                if (count == 3 && IsValidTriangle(attrib, mesh, indexOffset)) materialGroups[GetMaterialIndex(mesh, f, materialCount)].indexCount += 3;
                indexOffset += count;
            }
        }

        size_t indexCount = 0;
        for (auto& group : materialGroups)
        {
            group.indexOffset = indexCount;
            indexCount += group.indexCount;
        }
        return indexCount;
    }

    struct OBJVertexHash
    {
        // Hash function for a tuple of three integers (position index, normal index, texcoord index)
        // Needed for deduplicating vertices when loading OBJ files using an unordered_map
        size_t operator()(const std::tuple<int, int, int>& key) const
        {
            const int position = std::get<0>(key);
            const int normal = std::get<1>(key);
            const int texcoord = std::get<2>(key);

            return utils::mixIntegers(static_cast<uint32_t>(position), static_cast<uint32_t>(normal), static_cast<uint32_t>(texcoord));
        }
    };
}

HRESULT Scene::LoadOBJ(std::wstring filePath)
{
    Clear();

    tinyobj::ObjReader reader;

    if (!reader.ParseFromFile(utils::wstringToString(filePath)))
    {
        if (!reader.Error().empty())
        {
            utils::printMessage(reader.Error());
        }

        return E_FAIL;
    }

    if (!reader.Warning().empty())
    {
        utils::printMessage(reader.Warning());
    }

    auto& attrib = reader.GetAttrib();
    auto& shapes = reader.GetShapes();
    const auto& objMaterials = reader.GetMaterials();

    std::unordered_map<std::wstring, int> textureMap;
    textureMap.reserve(objMaterials.size() * 2); //< Base color and emissive textures
    auto loadColorTexture = [&](const std::string& name) -> int
    {
        if (name.empty()) return -1;

        const std::wstring textureName = utils::stringToWstring(name);
        const bool absolutePath = textureName[0] == L'/' || textureName[0] == L'\\' || (textureName.size() > 1 && textureName[1] == L':');
        const std::wstring path = utils::resolvePath(absolutePath ? textureName : utils::getPath(filePath) + textureName);
        auto found = textureMap.find(path);
        if (found != textureMap.end()) return found->second;

        int textureIndex = -1;
        Image image;
        if (texture_helpers::loadImageFile(path, image))
        {
            const int imageIndex = int(images.size());
            images.push_back(std::move(image));
            textureIndex = int(textures.size());
            textures.push_back({ imageIndex, true });
        }

        if (textureIndex < 0) utils::printMessage("Could not load OBJ texture: " + utils::wstringToString(path) + "\n");
        textureMap.emplace(path, textureIndex);
        return textureIndex;
    };

    materials.reserve(objMaterials.size() + 1);

    // Preserve material zero for faces without a valid MTL material.
    for (const auto& source : objMaterials)
    {
        Material material;
        const glm::vec3 diffuse(source.diffuse[0], source.diffuse[1], source.diffuse[2]);
        const glm::vec3 specular(source.specular[0], source.specular[1], source.specular[2]);
        const bool isMetal = glm::length(specular) > glm::length(diffuse) * 2.0f;
        material.properties.baseColor = glm::max(isMetal ? specular : diffuse, glm::vec3(0.0f));
        material.properties.metalness = isMetal ? 1.0f : 0.0f;
        material.properties.emissive = glm::max(glm::vec3(source.emission[0], source.emission[1], source.emission[2]), glm::vec3(0.0f));
        material.properties.roughness = std::sqrt(brdf::shininessToBeckmannAlpha(std::max(float(source.shininess), 0.0f)));
        material.properties.opacity = glm::clamp(float(source.dissolve), 0.0f, 1.0f);
        material.alphaMode = (material.properties.opacity < 1.0f) ? ALPHA_MODE_BLEND : ALPHA_MODE_OPAQUE;

        // Approximate classic Kd/Ks/Ns materials; Pm/Pr extensions are not imported here.
        // RGB transmittance cannot be copied to our scalar transmission; keep its default.
        material.baseColorTexture = loadColorTexture(source.diffuse_texname);
        material.emissiveTexture = loadColorTexture(source.emissive_texname);
        // Texture maps replace legacy Kd/Ke, matching the reference loader.
        if (material.baseColorTexture >= 0) material.properties.baseColor = glm::vec3(1.0f);
        if (material.emissiveTexture >= 0) material.properties.emissive = glm::vec3(1.0f);
        materials.push_back(material);
    }

    // Share vertices and group triangle indices by material.
    std::vector<obj_helpers::MaterialGroup> materialGroups;
    const size_t indexCount = obj_helpers::PrescanScene(attrib, shapes, objMaterials.size(), materialGroups);
    if (indexCount == 0) return S_OK;

    // Allocate the final index buffer once; each material writes into its own range.
    indices.resize(indexCount);
    const size_t geometryCount = std::count_if(materialGroups.begin(), materialGroups.end(), [](const obj_helpers::MaterialGroup& group) { return group.indexCount != 0; });
    geometries.reserve(geometryCount);
    instances.reserve(geometryCount);

    // Map OBJ's separate position/normal/texture coordinate indices to our unified vertices
    std::unordered_map<std::tuple<int, int, int>, glm::uint, obj_helpers::OBJVertexHash> vertexMap;
    vertexMap.reserve(attrib.vertices.size() / 3);

    // Loop over shapes
    for (size_t s = 0; s < shapes.size(); s++)
    {
        // Loop over faces (polygon)
        size_t index_offset = 0;
        for (size_t f = 0; f < shapes[s].mesh.num_face_vertices.size(); f++)
        {
            const size_t fv = size_t(shapes[s].mesh.num_face_vertices[f]);

            if (index_offset > shapes[s].mesh.indices.size() || fv > shapes[s].mesh.indices.size() - index_offset)
            {
                utils::printMessage("OBJ shape has an incomplete face index range.\n");
                break;
            }

            if (fv == 3)
            {
                // Validate the whole triangle before adding anything to the scene
                if (!obj_helpers::IsValidTriangle(attrib, shapes[s].mesh, index_offset))
                {
                    utils::printMessage("OBJ face references an invalid vertex attribute and was ignored.\n");
                    index_offset += fv;
                    continue;
                }

                // Resolve material index, use default (at index 0) if none was specified
                const glm::uint materialIndex = obj_helpers::GetMaterialIndex(shapes[s].mesh, f, objMaterials.size());
                
                // Get a material group for this face
                obj_helpers::MaterialGroup& group = materialGroups[materialIndex];

                glm::uint faceIndices[3];
                bool faceHasNormals = true;

                // Read the triangle vertices
                for (size_t vertexIndex = 0; vertexIndex < 3; vertexIndex++)
                {
                    const auto& source = shapes[s].mesh.indices[index_offset + vertexIndex];
                    const auto key = std::make_tuple(source.vertex_index, source.normal_index, source.texcoord_index);

                    if (source.normal_index < 0)
                    {
                        faceHasNormals = false;
                    }

                    auto found = vertexMap.find(key);
                    if (found == vertexMap.end())
                    {
                        Vertex vertex = {};
                        vertex.uv = glm::vec2(0.0f);

                        const size_t p = size_t(source.vertex_index) * 3;
                        vertex.position = glm::vec3(attrib.vertices[p], attrib.vertices[p + 1], attrib.vertices[p + 2]);

                        if (source.normal_index >= 0)
                        {
                            const size_t n = size_t(source.normal_index) * 3;
                            vertex.normal = glm::vec3(attrib.normals[n], attrib.normals[n + 1], attrib.normals[n + 2]);
                        }

                        if (source.texcoord_index >= 0)
                        {
                            const size_t t = size_t(source.texcoord_index) * 2;
                            vertex.uv = glm::vec2(attrib.texcoords[t], 1.0f - attrib.texcoords[t + 1]);
                        }

                        sceneMax = glm::max(sceneMax, vertex.position);
                        sceneMin = glm::min(sceneMin, vertex.position);

                        found = vertexMap.emplace(key, glm::uint(vertices.size())).first;
                        vertices.push_back(vertex);
                    }

                    faceIndices[vertexIndex] = found->second;

                    // Update group (instance) bounds
                    const glm::vec3& position = vertices[found->second].position;
                    group.boundsMin = glm::min(group.boundsMin, position);
                    group.boundsMax = glm::max(group.boundsMax, position);
                }

                // Add normals if not present
                if (!faceHasNormals)
                {
                    // Calculate geometry normal of this triangle
                    const glm::vec3 faceNormal = glm::cross(vertices[faceIndices[1]].position - vertices[faceIndices[0]].position, vertices[faceIndices[2]].position - vertices[faceIndices[0]].position);

                    // Accumulate geometry normals for vertices without normals in the OBJ
                    for (size_t vertexIndex = 0; vertexIndex < 3; vertexIndex++)
                    {
                        const auto& source = shapes[s].mesh.indices[index_offset + vertexIndex];

                        if (source.normal_index < 0)
                        {
                            vertices[faceIndices[vertexIndex]].normal += faceNormal;
                        }
                    }
                }

                // Our raytracing and light extraction use cross(C - A, B - A).
                // Reverse OBJ winding so one-sided emitters retain their outward normal.
                const size_t destination = group.indexOffset + group.indicesWritten;
                indices[destination] = faceIndices[0];
                indices[destination + 1] = faceIndices[2];
                indices[destination + 2] = faceIndices[1];
                group.indicesWritten += 3;
            }

            index_offset += fv;
        }
    }

    // Normalize vertex normals
    for (Vertex& vertex : vertices)
    {
        if (glm::dot(vertex.normal, vertex.normal) > 0.0f)
        {
            vertex.normal = glm::normalize(vertex.normal);
        }
    }

    // Create an instance per material
    for (size_t materialIndex = 0; materialIndex < materialGroups.size(); materialIndex++)
    {
        const auto& group = materialGroups[materialIndex];
        if (group.indexCount == 0) continue;

        Geometry geometry;
        geometry.vertexOffset = 0;
        geometry.vertexCount = glm::uint(vertices.size());
        geometry.indexOffset = glm::uint(group.indexOffset);
        geometry.indexCount = glm::uint(group.indexCount);
        geometry.boundsMin = group.boundsMin;
        geometry.boundsMax = group.boundsMax;
        geometry.isOpaque = materials[materialIndex].alphaMode == ALPHA_MODE_OPAQUE;

        instances.push_back({ glm::uint(geometries.size()), glm::uint(materialIndex), glm::mat4(1.0f) });
        geometries.push_back(geometry);
    }

    return S_OK;
}

namespace gltf_helpers
{
    struct AccessorView
    {
        const unsigned char* bytes = nullptr;
        size_t stride = 0;
        size_t count = 0;
        size_t elementSize = 0;
    };

    // Grab data pointer and stride for a given accessor. Returns false if the accessor is invalid or sparse.
    // Also performs validation of the accessor's buffer view and buffer, and checks that the accessor's count is valid given the available bytes.
    bool GetView(const tinygltf::Model& model, int index, int type, int componentType, AccessorView& result)
    {
        // Fail if the accessor is invalid
        if (index < 0 || size_t(index) >= model.accessors.size()) return false;

        // Fail if the accessor's type or component type does not match the expected values, or if the accessor is sparse, or if the buffer view is invalid
        // TODO: support sparse accessors by reading the sparse data and merging it with the buffer view data?
        const auto& accessor = model.accessors[index];
        if (accessor.type != type || accessor.componentType != componentType || accessor.sparse.isSparse ||
            accessor.bufferView < 0 || size_t(accessor.bufferView) >= model.bufferViews.size()) return false;

        // Fail if the buffer view's buffer is invalid
        const auto& view = model.bufferViews[accessor.bufferView];
        if (view.buffer < 0 || size_t(view.buffer) >= model.buffers.size()) return false;

        // Figure out element size and stride (those can be different for packed data)
        const int elementSize = tinygltf::GetComponentSizeInBytes(accessor.componentType) * tinygltf::GetNumComponentsInType(accessor.type);
        const int stride = accessor.ByteStride(view);

        // Validate stored values against the buffer size and accessor count. The accessor's byteOffset is relative to the buffer view's byteOffset, and the buffer view's byteLength must be large enough to hold the accessor's data.
        const auto& buffer = model.buffers[view.buffer].data;
        if (elementSize <= 0 || stride < elementSize || view.byteOffset > buffer.size() || view.byteLength > buffer.size() - view.byteOffset || accessor.byteOffset > view.byteLength) return false;

        // Fail if there is a risk of out of-bounds access
        const size_t available = view.byteLength - accessor.byteOffset;
        if (accessor.count && (size_t(elementSize) > available || accessor.count - 1 > (available - size_t(elementSize)) / size_t(stride))) return false;

        // Construct AccessorView helper to read data
        result.bytes = (buffer.data() + view.byteOffset + accessor.byteOffset);
        result.count = accessor.count;
        result.stride = size_t(stride);
        result.elementSize = size_t(elementSize);

        return true;
    }

    struct SceneCapacity
    {
        size_t vertices = 0;
        size_t indices = 0;
        size_t geometries = 0;
        size_t instances = 0;
        size_t cameras = 0;
        std::vector<uint8_t> usedMaterials;
    };

    // Prescan the scene to figure out memory requirements. Shared meshes contribute geometry once, but instances per node.
    bool PrescanScene(const tinygltf::Model& model, int sceneIndex, SceneCapacity& capacity, std::vector<uint8_t>& visited)
    {
        capacity = {};
        capacity.usedMaterials.assign(model.materials.size(), 0);

        // Caches for marking nodes and meshes as visited
        visited.assign(model.nodes.size(), 0);

        std::vector<uint8_t> countedMeshes(model.meshes.size(), 0);
        bool countsValid = true;

        // Keep counts within container capacity and the integer types used for offsets and IDs.
        auto addCount = [&](size_t& total, size_t count, size_t limit)
        {
            if (count > limit - total) countsValid = false;
            else total += count;
        };

        // Helper to visit scene graph node and count it's memory requirements
        std::function<void(int)> countNode = [&](int nodeIndex)
        {
            if (!countsValid || nodeIndex < 0 || size_t(nodeIndex) >= model.nodes.size() || visited[nodeIndex]) return;

            // Mark node as visited
            visited[nodeIndex] = 1;

            // Grab the node
            const auto& node = model.nodes[nodeIndex];

            // Mark the camera
            if (node.camera >= 0 && size_t(node.camera) < model.cameras.size())
            {
                const auto& camera = model.cameras[node.camera];
                if (camera.type == "perspective" && camera.perspective.yfov > 0) addCount(capacity.cameras, 1, std::vector<Scene::Camera>().max_size());
            }

            // Mark the mesh node
            if (node.mesh >= 0 && size_t(node.mesh) < model.meshes.size())
            {
                for (const auto& primitive : model.meshes[node.mesh].primitives)
                {
                    if (primitive.mode != TINYGLTF_MODE_TRIANGLES) continue;
                    
                    // Mark the instance needed
                    addCount(capacity.instances, 1, std::min<size_t>(std::vector<Scene::Instance>().max_size(), std::numeric_limits<glm::uint>::max()));
                    
                    // Skip counting geometry if already counted
                    if (countedMeshes[node.mesh]) continue;

                    // Grab the buffers and read their capacities
                    const auto position = primitive.attributes.find("POSITION");
                    AccessorView positions;
                    if (position == primitive.attributes.end() || !GetView(model, position->second, TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT, positions)) continue;

                    size_t indexCount = positions.count;
                    if (primitive.indices != -1)
                    {
                        if (primitive.indices < 0 || size_t(primitive.indices) >= model.accessors.size()) continue;
                        const auto& accessor = model.accessors[primitive.indices];
                        if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE && accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT && accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) continue;
                        AccessorView indexView;
                        if (!GetView(model, primitive.indices, TINYGLTF_TYPE_SCALAR, accessor.componentType, indexView)) continue;
                        indexCount = indexView.count;
                    }

                    if (primitive.material >= 0 && size_t(primitive.material) < model.materials.size()) capacity.usedMaterials[primitive.material] = 1;

                    // Validate accessor spans without reading vertex/index values; conversion may reject more data later.
                    addCount(capacity.vertices, positions.count, std::min<size_t>(std::vector<Vertex>().max_size(), std::numeric_limits<glm::uint>::max()));
                    addCount(capacity.indices, indexCount, std::min<size_t>(std::vector<glm::uint>().max_size(), std::numeric_limits<glm::uint>::max()));
                    addCount(capacity.geometries, 1, std::min<size_t>(std::vector<Scene::Geometry>().max_size(), std::numeric_limits<int>::max()));
                }

                // Mark mesh as visited
                countedMeshes[node.mesh] = 1;
            }

            // Recurse into children
            for (int child : node.children)
            {
                countNode(child);
            }
        };

        // Iterate scene graph from roots
        for (int root : model.scenes[sceneIndex].nodes)
        {
            countNode(root);
        }

        return countsValid;
    }

    // Just read templated type from view and index
    template<class T> T ReadFromView(const AccessorView& view, size_t i)
    {
        T result;
        memcpy(&result, view.bytes + i * view.stride, sizeof(T));
        return result;
    }

    // Helper to read UVs. We'll convert all types to 32-bit floats here
    glm::vec2 ReadUVsFromView(const AccessorView& view, size_t i, int componentType)
    {
        // Easy path for floats
        if (componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) return ReadFromView<glm::vec2>(view, i);

        // Raw pointer to coordinates
        const unsigned char* bytes = view.bytes + i * view.stride;

        // If 8-bit tex. coords, map unsigned chars to float
        if (componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
        {
            return glm::vec2(bytes[0], bytes[1]) * (1.0f / 255.0f);
        }

        // If 16-bit coordinates, map them to floats here
        if (componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
        {
            uint16_t u, v;
            memcpy(&u, bytes, sizeof(u));
            memcpy(&v, bytes + sizeof(u), sizeof(v));
            return glm::vec2(u, v) * (1.0f / 65535.0f);
        }

        // Could not read
        return glm::vec2(0, 0);
    }

    // Helper to read and convert indices to our format (32-bit)
    bool ReadIndicesFromView(const tinygltf::Model& model, const tinygltf::Primitive& primitive, size_t vertexCount, std::vector<glm::uint>& result, glm::uint& indexOffset, glm::uint& indexCount)
    {
        // Fail on invalid buffer index
        if (primitive.indices < -1) return false;

        const size_t firstIndex = result.size();
        size_t count = 0;

        if (primitive.indices == -1)
        {
            // Primitive is non-indexed, generate a trivial index buffer
            result.resize(firstIndex + vertexCount);
            for (size_t i = 0; i < vertexCount; i++) result[firstIndex + i] = glm::uint(i);
            count = vertexCount;
        }
        else // Indexed mesh
        {
            // Fail on invalid buffer index
            if (primitive.indices >= int(model.accessors.size())) return false;

            const auto& accessor = model.accessors[primitive.indices];

            // Validate component type
            if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
                accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
                accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) return false;

            // Grab the view
            AccessorView view;
            if (!GetView(model, primitive.indices, TINYGLTF_TYPE_SCALAR, accessor.componentType, view)) return false;

            count = view.count;

            // Make room for new indices
            result.resize(firstIndex + count);

            // Read and convert indices
            for (size_t i = 0; i < count; i++)
            {
                glm::uint index = 0;

                // Convert index to 32-bit
                if (view.elementSize == 1)
                {
                    index = ReadFromView<uint8_t>(view, i);
                }
                else if (view.elementSize == 2)
                {
                    index = ReadFromView<uint16_t>(view, i);
                }
                else
                {
                    index = ReadFromView<uint32_t>(view, i);
                }

                // Fail on invalid index
                if (index >= vertexCount)
                {
                    result.resize(firstIndex); //< Forget indices added before failure
                    return false;
                }

                result[firstIndex + i] = index;
            }
        }

        // Validate the count
        if (count == 0 || (count % 3 != 0))
        {
            result.resize(firstIndex); //< Forget indices added before failure
            return false;
        }

        // Report indices offset and count
        indexOffset = glm::uint(firstIndex);
        indexCount = glm::uint(count);

        return true;
    }

    glm::mat4 ReadTransform(const tinygltf::Node& node)
    {
        // Use matrix if available
        if (node.matrix.size() == 16)
        {
            glm::mat4 matrix(1.0f);
            for (int i = 0; i < 16; i++) matrix[i / 4][i % 4] = float(node.matrix[i]);
            return matrix;
        }

        // Read Translation, Rotation, Scale (TRS) if available, otherwise use defaults
        const bool hasTranslation = node.translation.size() == 3;
        const bool hasRotation = node.rotation.size() == 4;
        const bool hasScale = node.scale.size() == 3;

        const glm::vec3 t = hasTranslation ? glm::vec3(node.translation[0], node.translation[1], node.translation[2]) : glm::vec3(0.0f);
        const glm::vec3 s = hasScale ? glm::vec3(node.scale[0], node.scale[1], node.scale[2]) : glm::vec3(1.0f);
        const glm::quat q = hasRotation ? glm::quat(float(node.rotation[3]), float(node.rotation[0]), float(node.rotation[1]), float(node.rotation[2])) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

        // Compose the transform matrix from TRS components
        return glm::scale(glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(q), s);
    }

    // Transform must have 4x4 matrix or valid TRS components (translation, rotation, scale)
    bool HasValidTransform(const tinygltf::Node& node)
    {
        return (node.matrix.empty() || node.matrix.size() == 16) &&
            (node.translation.empty() || node.translation.size() == 3) &&
            (node.rotation.empty() || node.rotation.size() == 4) &&
            (node.scale.empty() || node.scale.size() == 3);
    }

    // Read min/max values from an accessor
    bool ReadMinMax(const tinygltf::Accessor& accessor, glm::vec3& minValues, glm::vec3& maxValues)
    {
        if (accessor.minValues.size() != 3 || accessor.maxValues.size() != 3)
        {
            return false;
        }

        minValues = glm::vec3(float(accessor.minValues[0]), float(accessor.minValues[1]), float(accessor.minValues[2]));
        maxValues = glm::vec3(float(accessor.maxValues[0]), float(accessor.maxValues[1]), float(accessor.maxValues[2]));

        if (!std::isfinite(minValues.x) ||
            !std::isfinite(minValues.y) ||
            !std::isfinite(minValues.z) ||
            !std::isfinite(maxValues.x) ||
            !std::isfinite(maxValues.y) ||
            !std::isfinite(maxValues.z))
        {
            return false;
        }

        return (minValues.x <= maxValues.x) && (minValues.y <= maxValues.y) && (minValues.z <= maxValues.z);
    }

    bool ValidUVComponentType(int componentType, bool normalized)
    {
        if (componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
        {
            return true;
        }

        return normalized &&(componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE || componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
    }

    bool ProcessGLTFPrimitive(const tinygltf::Model& model, const tinygltf::Primitive& primitive, std::vector<Vertex>& vertices, std::vector<glm::uint>& indices, std::vector<Scene::Geometry>& geometries, int& geometryIndex)
    {
        // See if geometry has positions, normals and UVs
        const auto position = primitive.attributes.find("POSITION");
        const bool hasPositions = (position != primitive.attributes.end());

        const auto normal = primitive.attributes.find("NORMAL");
        const bool hasNormal = (normal != primitive.attributes.end());

        const auto uv = primitive.attributes.find("TEXCOORD_0");
        const bool hasUV = (uv != primitive.attributes.end());

        // Only process geometry that has positions
        if (!hasPositions) return false;

        AccessorView uvs;

        // Access the positions buffer and validate it
        AccessorView positions;
        if (!GetView(model, position->second, TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT, positions)) return false;

        // Normals are optional, but if present they must match the number of positions
        AccessorView normals;
        if (hasNormal)
        {
            if (!GetView(model, normal->second, TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT, normals) ||
                normals.count != positions.count) return false;
        }

        int uvComponent = TINYGLTF_COMPONENT_TYPE_FLOAT;
        if (hasUV)
        {
            // Fail on invalid UV accessor index
            if (uv->second < 0 || size_t(uv->second) >= model.accessors.size()) return false;
            const auto& uvAccessor = model.accessors[uv->second];

            // Grab the UV type (we'll convert this to floats)
            uvComponent = uvAccessor.componentType;

            // Fail on unsupported UV type
            if (!ValidUVComponentType(uvComponent, uvAccessor.normalized)) return false;

            // UVs are optional, but if present they must match the number of positions
            if (!GetView(model, uv->second, TINYGLTF_TYPE_VEC2, uvComponent, uvs) ||
                uvs.count != positions.count) return false;
        }

        // Fail on invalid indices buffer index
        if (primitive.indices >= int(model.accessors.size())) return false;

        // Prepare a new Geometry object
        // We will push its vertices and indices onto corresponding lists
        Scene::Geometry geometry;
        geometry.vertexOffset = glm::uint(vertices.size());
        geometry.vertexCount = glm::uint(positions.count);

        // Read the indices into array
        if (!ReadIndicesFromView(model, primitive, positions.count, indices, geometry.indexOffset, geometry.indexCount)) return false;

        // Read geometry bounds if available
        const auto& positionAccessor = model.accessors[position->second];
        const bool hasAccessorBounds = ReadMinMax(positionAccessor, geometry.boundsMin, geometry.boundsMax);

        // Read and convert vertices into our format
        vertices.resize(vertices.size() + positions.count);

        for (size_t vertexIndex = 0; vertexIndex < positions.count; vertexIndex++)
        {
            Vertex& vertex = vertices[geometry.vertexOffset + vertexIndex];

            vertex.position = ReadFromView<glm::vec3>(positions, vertexIndex);
            vertex.normal = hasNormal ? ReadFromView<glm::vec3>(normals, vertexIndex) : glm::vec3(0.0f);
            vertex.uv = hasUV ? ReadUVsFromView(uvs, vertexIndex, uvComponent) : glm::vec2(0.0f);

            // Calculate geometry bounds if not provided in GLTF file
            if (!hasAccessorBounds)
            {
                geometry.boundsMin = glm::min(geometry.boundsMin, vertex.position);
                geometry.boundsMax = glm::max(geometry.boundsMax, vertex.position);
            }
		}

		// Calculate normals if GLTF does not contain them
        const bool generateSmoothNormals = false;
		if (!hasNormal && generateSmoothNormals)
		{
			for (size_t i = 0; i < geometry.indexCount; i += 3)
			{
				const glm::uint a = indices[geometry.indexOffset + i + 0];
				const glm::uint b = indices[geometry.indexOffset + i + 1];
				const glm::uint c = indices[geometry.indexOffset + i + 2];

				Vertex& vertexA = vertices[geometry.vertexOffset + a];
				Vertex& vertexB = vertices[geometry.vertexOffset + b];
				Vertex& vertexC = vertices[geometry.vertexOffset + c];

				const glm::vec3 faceNormal = glm::cross(vertexB.position - vertexA.position, vertexC.position - vertexA.position);

				// Accumulate adjacent face normals into each vertex (unnormalized)
				vertexA.normal += faceNormal;
				vertexB.normal += faceNormal;
				vertexC.normal += faceNormal;
			}

			// Normalize the normals
			for (size_t v = 0; v < positions.count; v++)
			{
				float3& n = vertices[geometry.vertexOffset + v].normal;
				if (glm::dot(n, n) > 0.0f) n = glm::normalize(n);
			}
		}

		// Add new geometry
        geometryIndex = int(geometries.size());
        geometries.push_back(geometry);

        return true;
    }

    float tryGetExtensionValue(const tinygltf::ExtensionMap& extensions, const std::string& extensionName, const std::string& key, float defaultValue)
    {
        float result = defaultValue;

        const auto extension = extensions.find(extensionName);
        if (extension != extensions.end() && extension->second.Has(key))
        {
            const auto& value = extension->second.Get(key);
            if (value.IsNumber())
            {
                result = float(value.GetNumberAsDouble());
            }
        }

        return result;
    }
}

HRESULT Scene::LoadGLTF(std::wstring filePath)
{
    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(texture_helpers::deferImageLoad, nullptr);
    tinygltf::Model model;
    std::string errors, warnings;

    // TinyGLTF works with regular strings
    const std::string path = utils::wstringToString(filePath);

    // Load either GLB or GLTF using TinyGLTF
    bool loaded = false;
    if (utils::hasExtension(filePath, L"glb"))
    {
        loaded = loader.LoadBinaryFromFile(&model, &errors, &warnings, path);
    }
    else if (utils::hasExtension(filePath, L"gltf"))
    {
        loaded = loader.LoadASCIIFromFile(&model, &errors, &warnings, path);
    }

    // Report warnings if any
    if (!warnings.empty()) utils::printMessage(warnings);

    // Fail if failed
    if (!loaded) {
        utils::printMessage(errors);
        return E_FAIL;
    }

    // Construct a scene from GLTF data
    {
        // Validate the default scene index, if specified
        if (model.defaultScene >= int(model.scenes.size()))
        {
            utils::printMessage("glTF has no valid scene.\n");
            return E_FAIL;
        }

        // There is valid GLTF data, we can construct a scene from it. Clear any existing data first.
        Clear();

        // glTF defaults to a metallic material when no material is specified.
        materials[0].properties.metalness = 1.0f;

        if (model.scenes.empty()) return S_OK;

        // Grab the default scene
        const int sceneIndex = model.defaultScene < 0 ? 0 : model.defaultScene;

        // Prescan the scene to figure out how much memory we need to allocate for it
        gltf_helpers::SceneCapacity capacity;
        std::vector<uint8_t> visited;
        if (!gltf_helpers::PrescanScene(model, sceneIndex, capacity, visited))
        {
            utils::printMessage("glTF scene exceeds container capacity.\n");
            return E_FAIL;
        }

        // Pre-allocate enough memory for the whole scene
        vertices.reserve(capacity.vertices);
        indices.reserve(capacity.indices);
        geometries.reserve(capacity.geometries);
        instances.reserve(capacity.instances);
        cameras.reserve(capacity.cameras);
        materials.reserve(materials.size() + model.materials.size());
        std::fill(visited.begin(), visited.end(), 0);

        // Parse materials
        for (const auto& material : model.materials)
        {
            // Push new material
            materials.emplace_back();
            Material& converted = materials.back();

            // We use PBR material properties
            const auto& pbr = material.pbrMetallicRoughness;

            // Parse base color factor
            converted.properties.baseColor = glm::vec3(pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2]);
            
            // Parse opacity mode and settings
            converted.alphaMode = (material.alphaMode == "BLEND") ? ALPHA_MODE_BLEND : ((material.alphaMode == "MASK") ? ALPHA_MODE_MASK : ALPHA_MODE_OPAQUE);
            converted.alphaCutoff = float(material.alphaCutoff);
            converted.properties.opacity = float(pbr.baseColorFactor[3]);

            // Parse metallic-roughness factors
            converted.properties.metalness = float(pbr.metallicFactor);
            converted.properties.roughness = float(pbr.roughnessFactor);

            // Grab emissive factor. Emissive factor is multiplied by emissive strength if the extension is present
            float emissiveStrength = gltf_helpers::tryGetExtensionValue(material.extensions, "KHR_materials_emissive_strength", "emissiveStrength", 1.0f);
            converted.properties.emissive = glm::vec3(material.emissiveFactor[0], material.emissiveFactor[1], material.emissiveFactor[2]) * emissiveStrength;

            // Grab material textures
            converted.baseColorTexture = texture_helpers::getSupportedTextureIndex(pbr.baseColorTexture);
            converted.metallicRoughnessTexture = texture_helpers::getSupportedTextureIndex(pbr.metallicRoughnessTexture);
            converted.emissiveTexture = texture_helpers::getSupportedTextureIndex(material.emissiveTexture);
        }

        // Parse textures
        {
            // Plan unique image sources and sampling views
            std::unordered_map<texture_helpers::ImageKey, int, texture_helpers::ImageKeyHash> imageMap;
            std::unordered_map<uint64_t, int> textureMap;
            imageMap.reserve(model.images.size());
            textureMap.reserve(model.textures.size() * 2);

            std::vector<texture_helpers::ImageKey> imageSources;
            std::vector<Texture> textureViews;
            imageSources.reserve(model.images.size());
            textureViews.reserve(model.textures.size() * 2); //< Each texture may need linear and sRGB views

            auto resolveTexture = [&](int gltfTextureIndex, bool isSrgb)
            {
                // Validate and fetch the texture
                if (gltfTextureIndex < 0 || size_t(gltfTextureIndex) >= model.textures.size()) return -1;
                const auto& texture = model.textures[gltfTextureIndex];
                if (texture.source < 0 || size_t(texture.source) >= model.images.size()) return -1;
                
                // Calculate hashing key for the image
                texture_helpers::ImageKey imageKey;
                if (!texture_helpers::getImageKey(filePath, model, texture.source, imageKey)) return -1;

                // Register new texture based in its key
                const auto insertedImage = imageMap.emplace(imageKey, int(imageSources.size()));
                if (insertedImage.second)
                {
                    imageSources.push_back(imageKey);
                }

                // Grab index of the image in imageSources
                const int imageIndex = insertedImage.first->second;

                // Push new view to this texture onto a list, wiht resolved sRGB flag
                // Pack the image index and sampling color space into one lookup key.
                const uint64_t textureKey = (uint64_t(imageIndex) << 1) | uint64_t(isSrgb);
                const auto insertedView = textureMap.emplace(textureKey, int(textureViews.size()));
                if (insertedView.second)
                {
                    textureViews.push_back({ imageIndex, isSrgb });
                }

                // Return index to the view
                return insertedView.first->second;
            };

            // Resolve texture indices as indices for views to textures
            for (size_t i = 1; i < materials.size(); i++)
            {
                Material& material = materials[i];
                if (!capacity.usedMaterials[i - 1])
                {
                    material.baseColorTexture = material.metallicRoughnessTexture = material.emissiveTexture = -1;
                    continue;
                }
                material.baseColorTexture = resolveTexture(material.baseColorTexture, true);
                material.metallicRoughnessTexture = resolveTexture(material.metallicRoughnessTexture, false);
                material.emissiveTexture = resolveTexture(material.emissiveTexture, true);
            }

            images.reserve(imageSources.size());
            textures.reserve(textureViews.size());
            std::vector<int> imageRemap(imageSources.size(), -1);

            // Load images in parallel after they were deduplicated and identified as used
            std::vector<Image> decodedImages(imageSources.size());
            std::atomic<size_t> nextImage{ 0 };
            auto loadImages = [&]()
            {
                while (true)
                {
                    // Grab next texture to load
                    const size_t i = nextImage.fetch_add(1, std::memory_order_relaxed);
                    if (i >= imageSources.size()) return; //< Exit once all are processed

                    const auto& source = imageSources[i];
                    Image& image = decodedImages[i];

                    if (!source.imagePath.empty())
                    {
                        texture_helpers::loadImageFile(source.imagePath, image);
                    }
                    else if (source.buffer >= 0)
                    {
                        texture_helpers::loadImageMemory(model.buffers[source.buffer].data.data() + source.byteOffset, source.byteLength, image);
                    }
                    else
                    {
                        texture_helpers::loadImageMemory(source.encodedData, source.encodedSize, image);
                    }
                }
            };

            // Start workers for texture loading
            const size_t workerCount = std::min<size_t>(imageSources.size(), std::min(4u, std::max(1u, std::thread::hardware_concurrency())));
            std::vector<std::future<void>> workers;
            for (size_t i = 1; i < workerCount; i++) workers.push_back(std::async(std::launch::async, loadImages));
            
            loadImages();

            // Wait until textures are loaded
            for (auto& worker : workers) worker.get();

            // Preserve source order for texture indices and error messages.
            for (size_t i = 0; i < imageSources.size(); i++)
            {
                const auto& source = imageSources[i];
                Image& image = decodedImages[i];

                // Fail if we could not load the image
                if (!image.texels)
                {
                    utils::printMessage("Could not load glTF image: " + (source.imagePath.empty() ? std::to_string(i) : utils::wstringToString(source.imagePath)) + "\n");
                    continue;
                }

                // Mark the new index of loaded/resolved texture to ensure continuous array
                imageRemap[i] = int(images.size());

                images.push_back(std::move(image));
            }

            // Fix indices of images in views after remaping
            std::vector<int> textureRemap(textureViews.size(), -1);
            for (size_t i = 0; i < textureViews.size(); i++)
            {
                Texture view = textureViews[i];
                view.imageIndex = imageRemap[view.imageIndex];
                if (view.imageIndex < 0) continue;

                textureRemap[i] = int(textures.size());
                textures.push_back(view);
            }

            // Fix texture view indices in materials after remapping
            for (Material& material : materials)
            {
                if (material.baseColorTexture >= 0) material.baseColorTexture = textureRemap[material.baseColorTexture];
                if (material.metallicRoughnessTexture >= 0) material.metallicRoughnessTexture = textureRemap[material.metallicRoughnessTexture];
                if (material.emissiveTexture >= 0) material.emissiveTexture = textureRemap[material.emissiveTexture];
            }
        }

        // Compute each mesh's starting offset in the flat per-primitive geometry lookup.
        std::vector<size_t> meshPrimitiveOffsets(model.meshes.size() + 1, 0);
        for (size_t m = 0; m < model.meshes.size(); m++)
        {
            const size_t count = model.meshes[m].primitives.size();
            meshPrimitiveOffsets[m + 1] = meshPrimitiveOffsets[m] + count;
        }

        // Maps each glTF mesh primitive to its Scene::geometries index.
        // One contiguous lookup allocation; -1 means not imported yet, -2 means rejected.
        // Nodes referencing the same mesh reuse these geometries and create new instances.
        std::vector<int> geometryMap;
        geometryMap.assign(meshPrimitiveOffsets.back(), -1);

        // Handedness helper
        const glm::mat4 handedness = glm::scale(glm::mat4(1.0f), glm::vec3(1, 1, -1));

        // Helper to process GLTF scene graph node
        std::function<void(int, const glm::mat4&)> visitGLTFNode = [&](int nodeIndex, const glm::mat4& parentTransform)
            {
                // Skip invalid nodes
                if (nodeIndex < 0 || size_t(nodeIndex) >= model.nodes.size() || visited[nodeIndex]) return;

                // Mark node as visited
                visited[nodeIndex] = 1;

                // Grab the node
                const auto& node = model.nodes[nodeIndex];

                // Validate the node transform
                const bool hasValidTransform = gltf_helpers::HasValidTransform(node);

                glm::mat4 nodeTransform = parentTransform;

                if (hasValidTransform)
                {
                    // Calculate transformation for this node
                    nodeTransform = parentTransform * gltf_helpers::ReadTransform(node);
                }
                else
                {
                    std::string warning = "glTF node ";
                    if (!node.name.empty()) warning += "\"" + node.name + "\" ";
                    warning += "(index " + std::to_string(nodeIndex) + ") has invalid transform data; using an identity local transform.\n";
                    utils::printMessage(warning);
                }

                // Process the node using its resolved transform
                {
                    // Parse camera if present
                    if (node.camera >= 0)
                    {
                        if (size_t(node.camera) < model.cameras.size())
                        {
                            const auto& camera = model.cameras[node.camera];

                            if (camera.type == "perspective" && camera.perspective.yfov > 0)
                            {
                                const glm::mat4 cameraTransform = handedness * nodeTransform;

                                cameras.emplace_back();
                                Camera& convertedCamera = cameras.back();

                                convertedCamera.position = glm::vec3(cameraTransform[3]);
                                convertedCamera.forward = glm::normalize(glm::vec3(cameraTransform * glm::vec4(0, 0, -1, 0)));
                                convertedCamera.up = glm::normalize(glm::vec3(cameraTransform * glm::vec4(0, 1, 0, 0)));
                                convertedCamera.fovY = glm::degrees(float(camera.perspective.yfov));
                            }
                        }
                    }

                    // Parse mesh if present
                    if (node.mesh >= 0)
                    {
                        if (size_t(node.mesh) < model.meshes.size())
                        {
                            const auto& mesh = model.meshes[node.mesh];
                            const auto& primitives = mesh.primitives;
                            for (size_t primitiveIndex = 0; primitiveIndex < primitives.size(); primitiveIndex++)
                            {
                                const auto& primitive = primitives[primitiveIndex];

                                const bool isValidPrimitive = (primitive.mode == TINYGLTF_MODE_TRIANGLES);

                                if (isValidPrimitive)
                                {
                                    // Assign a material
                                    glm::uint materialIndex = 0; //< Default material as default
                                    if ((primitive.material < int(model.materials.size()) && primitive.material >= 0))
                                    {
                                        materialIndex = primitive.material + 1; //< +1 to skip default material
                                    }
                                   
                                    // See if this geometry was already parsed
                                    int& geometryIndex = geometryMap[meshPrimitiveOffsets[node.mesh] + primitiveIndex];
                                    if (geometryIndex == -2) continue;
                                    if (geometryIndex == -1)
                                    {
                                        // If it wasn't, then process and create new geometry
                                        if (!gltf_helpers::ProcessGLTFPrimitive(model, primitive, vertices, indices, geometries, geometryIndex))
                                        {
                                            geometryIndex = -2;
                                            // Emit a message if we cannot load a geometry.
                                            std::string warning = "glTF mesh ";
                                            if (!mesh.name.empty()) warning += "\"" + mesh.name + "\" ";
                                            warning += "(index " + std::to_string(node.mesh) + "), primitive " + std::to_string(primitiveIndex) + " was not loaded because its geometry data is invalid or unsupported.\n";
                                            
                                            utils::printMessage(warning);
                                            continue;
                                        }
                                    }

                                    // Create the instance for this geometry with node transform
                                    if (geometryIndex >= 0)
                                    {
                                        const glm::mat4 instanceTransform = handedness * nodeTransform;
                                        Geometry& geometry = geometries[geometryIndex];
                                        geometry.isOpaque = (geometry.isOpaque && (materials[materialIndex].alphaMode == ALPHA_MODE_OPAQUE));

                                        instances.push_back({ glm::uint(geometryIndex), materialIndex, instanceTransform });

                                        // Update scene bounds for this instance
                                        utils::AABBUnionTransformed(sceneMin, sceneMax, geometry.boundsMin, geometry.boundsMax, instanceTransform);
                                    }
                                }
                            }
                        }
                    }
                }

                // Recurse into children
                for (int child : node.children) visitGLTFNode(child, nodeTransform);
            };

        // Traverse the scene graph from all roots
        for (int root : model.scenes[sceneIndex].nodes)
        {
            visitGLTFNode(root, glm::mat4(1.0f));
        }
    }

    return S_OK;
}
