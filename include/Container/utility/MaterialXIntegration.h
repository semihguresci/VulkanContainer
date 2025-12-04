#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <tiny_gltf.h>

#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>

#include <string>
#include <vector>

namespace utility::materialx {

class SlangMaterialXBridge {
public:
    SlangMaterialXBridge();

    MaterialX::DocumentPtr loadDocument(const std::string& filename);

    glm::vec4 extractBaseColor(const MaterialX::DocumentPtr& document) const;

    MaterialX::DocumentPtr createDocumentFromGltfMaterial(
        const tinygltf::Model& model, const tinygltf::Material& gltfMaterial,
        const std::string& materialName = "") const;

    std::vector<MaterialX::DocumentPtr> loadGltfMaterials(
        const std::string& filename) const;

private:
    static bool isBinaryGltf(const std::string& path);
    static MaterialX::Color3 toColor(const std::vector<double>& factor,
                                     const MaterialX::Color3& fallback);
    static std::string resolveTextureUri(const tinygltf::Model& model,
                                         int textureIndex);
};

} // namespace utility::materialx
