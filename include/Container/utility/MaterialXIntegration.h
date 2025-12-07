#pragma once

#include <Container/utility/MaterialManager.h>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <tiny_gltf.h>

#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace utility::materialx {

class SlangMaterialXBridge {
public:
    SlangMaterialXBridge();

    MaterialX::DocumentPtr loadDocument(const std::string& filename);

    glm::vec4 extractBaseColor(const MaterialX::DocumentPtr& document) const;

    glm::vec3 extractColorInput(const MaterialX::DocumentPtr& document,
                                const std::string& inputName,
                                const glm::vec3& defaultValue) const;

    float extractFloatInput(const MaterialX::DocumentPtr& document,
                            const std::string& inputName,
                            float defaultValue) const;

    std::string extractTextureFileForInput(
        const MaterialX::DocumentPtr& document,
        const std::string& inputName) const;

    MaterialX::DocumentPtr createDocumentFromGltfMaterial(
        const tinygltf::Model& model, const tinygltf::Material& gltfMaterial,
        const std::string& materialName = "") const;

    std::vector<MaterialX::DocumentPtr> loadGltfMaterials(
        const std::string& filename) const;

    std::vector<MaterialX::DocumentPtr> loadGltfMaterials(
        const tinygltf::Model& model) const;

    std::vector<uint32_t> loadTexturesForGltf(
        const tinygltf::Model& model, const std::filesystem::path& baseDir,
        utility::material::TextureManager& textureManager,
        const std::function<utility::material::TextureResource(
            const std::string&)>& textureLoader) const;

    void loadMaterialsForGltf(
        const tinygltf::Model& model,
        const std::vector<uint32_t>& imageToTexture,
        utility::material::MaterialManager& materialManager,
        uint32_t& defaultMaterialIndex) const;

private:
    static bool isBinaryGltf(const std::string& path);
    static MaterialX::Color3 toColor(const std::vector<double>& factor,
                                     const MaterialX::Color3& fallback);
    static std::string resolveTextureUri(const tinygltf::Model& model,
                                         int textureIndex);
};

} // namespace utility::materialx
