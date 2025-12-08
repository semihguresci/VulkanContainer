#include <Container/utility/MaterialXIntegration.h>
#include <MaterialXCore/Material.h>
#include <MaterialXCore/Util.h>
#include <MaterialXCore/Value.h>

#include <tiny_gltf.h>

#include <cctype>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
MaterialX::Color3 parseColorOrDefault(const MaterialX::ValuePtr& value,
                                      const MaterialX::Color3& fallback) {
  if (!value) {
    return fallback;
  }

  if (value->isA<MaterialX::Color3>()) {
    return value->asA<MaterialX::Color3>();
  }

  if (value->isA<MaterialX::Color4>()) {
    auto asColor4 = value->asA<MaterialX::Color4>();
    return MaterialX::Color3(asColor4[0], asColor4[1], asColor4[2]);
  }

  auto maybeColor =
      MaterialX::fromValueString<MaterialX::Color3>(value->getValueString());
  if (maybeColor != MaterialX::Color3()) {
    return maybeColor;
  }

  return fallback;
}

std::string resolveImageFileInput(const MaterialX::InputPtr& input) {
  if (!input) {
    return {};
  }

  auto node = input->getConnectedNode();
  if (!node) {
    return {};
  }

  if (node->getCategory() == "image") {
    if (auto fileInput = node->getInput("file")) {
      return fileInput->getValueString();
    }
  }

  for (const auto& childInput : node->getInputs()) {
    auto fileName = resolveImageFileInput(childInput);
    if (!fileName.empty()) {
      return fileName;
    }
  }

  return {};
}
}  // namespace

namespace utility::materialx {

SlangMaterialXBridge::SlangMaterialXBridge() = default;

MaterialX::DocumentPtr SlangMaterialXBridge::loadDocument(
    const std::string& filename) {
  auto document = MaterialX::createDocument();
  MaterialX::XmlReadOptions options;

  MaterialX::readFromXmlFile(document, filename, MaterialX::FileSearchPath(),
                             &options);
  document->importLibrary(MaterialX::createDocument());
  return document;
}

glm::vec4 SlangMaterialXBridge::extractBaseColor(
    const MaterialX::DocumentPtr& document) const {
  const MaterialX::Color3 defaultColor(1.0f, 1.0f, 1.0f);

  if (!document) {
    return glm::vec4(defaultColor[0], defaultColor[1], defaultColor[2], 1.0f);
  }

  // Look for standard surface shaders
  auto surfaceShaders = document->getNodesOfType("standard_surface");
  for (const auto& shader : surfaceShaders) {
    if (auto input = shader->getInput("base_color")) {
      auto baseColor = parseColorOrDefault(input->getValue(), defaultColor);
      return glm::vec4(baseColor[0], baseColor[1], baseColor[2], 1.0f);
    }
  }

  // Look for GLSL shaders
  auto glslShaders = document->getNodesOfType("glsl");
  for (const auto& shader : glslShaders) {
    std::vector<std::string> colorInputs = {"base_color", "baseColor",
                                            "diffuse_color"};
    for (const auto& inputName : colorInputs) {
      if (auto input = shader->getInput(inputName)) {
        auto baseColor = parseColorOrDefault(input->getValue(), defaultColor);
        return glm::vec4(baseColor[0], baseColor[1], baseColor[2], 1.0f);
      }
    }
  }

  return glm::vec4(defaultColor[0], defaultColor[1], defaultColor[2], 1.0f);
}

glm::vec3 SlangMaterialXBridge::extractColorInput(
    const MaterialX::DocumentPtr& document, const std::string& inputName,
    const glm::vec3& defaultValue) const {
  const MaterialX::Color3 defaultColor(defaultValue[0], defaultValue[1],
                                       defaultValue[2]);

  if (!document) {
    return defaultValue;
  }

  auto surfaceShaders = document->getNodesOfType("standard_surface");
  for (const auto& shader : surfaceShaders) {
    if (auto input = shader->getInput(inputName)) {
      auto color = parseColorOrDefault(input->getValue(), defaultColor);
      return glm::vec3(color[0], color[1], color[2]);
    }
  }

  return defaultValue;
}

float SlangMaterialXBridge::extractFloatInput(
    const MaterialX::DocumentPtr& document, const std::string& inputName,
    float defaultValue) const {
  if (!document) {
    return defaultValue;
  }

  auto surfaceShaders = document->getNodesOfType("standard_surface");
  for (const auto& shader : surfaceShaders) {
    if (auto input = shader->getInput(inputName)) {
      if (auto value = input->getValue()) {
        if (value->isA<float>()) {
          return value->asA<float>();
        }

        if (!value->getValueString().empty()) {
          return MaterialX::fromValueString<float>(value->getValueString());
        }
      }
    }
  }

  return defaultValue;
}

std::string SlangMaterialXBridge::extractTextureFileForInput(
    const MaterialX::DocumentPtr& document,
    const std::string& inputName) const {
  if (!document) {
    return {};
  }

  auto surfaceShaders = document->getNodesOfType("standard_surface");
  for (const auto& shader : surfaceShaders) {
    if (auto input = shader->getInput(inputName)) {
      const auto fileName = resolveImageFileInput(input);
      if (!fileName.empty()) {
        return fileName;
      }
    }
  }

  return {};
}

bool SlangMaterialXBridge::isBinaryGltf(const std::string& path) {
  const auto dotPos = path.find_last_of('.');
  if (dotPos == std::string::npos || dotPos + 1 >= path.size()) return false;

  std::string ext = path.substr(dotPos + 1);
  for (char& c : ext) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return ext == "glb";
}

MaterialX::Color3 SlangMaterialXBridge::toColor(
    const std::vector<double>& factor, const MaterialX::Color3& fallback) {
  if (factor.size() < 3) {
    return fallback;
  }

  float r = static_cast<float>(factor[0]);
  float g = static_cast<float>(factor[1]);
  float b = static_cast<float>(factor[2]);
  return MaterialX::Color3(r, g, b);
}

std::string SlangMaterialXBridge::resolveTextureUri(const tinygltf::Model& model,
                                                    int textureIndex) {
  if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size())) {
    return {};
  }

  const auto& texture = model.textures[textureIndex];
  if (texture.source >= 0 &&
      texture.source < static_cast<int>(model.images.size())) {
    const auto& image = model.images[texture.source];
    if (!image.uri.empty()) {
      return image.uri;
    }
    if (!image.name.empty()) {
      return image.name;
    }
    return "texture_" + std::to_string(texture.source);
  }

  return {};
}

MaterialX::DocumentPtr SlangMaterialXBridge::createDocumentFromGltfMaterial(
    const tinygltf::Model& model, const tinygltf::Material& gltfMaterial,
    const std::string& materialName) const {
  auto document = MaterialX::createDocument();
  const auto& pbr = gltfMaterial.pbrMetallicRoughness;
  const MaterialX::Color3 defaultColor(1.0f, 1.0f, 1.0f);
  const MaterialX::Color3 blackColor(0.0f, 0.0f, 0.0f);

  std::string shaderName = materialName.empty() ? "gltf_shader" : materialName + "_shader";
  auto shader = document->addNode("standard_surface", shaderName, "surfaceshader");

  auto baseInput = shader->addInput("base", "float");
  baseInput->setValue(1.0f);

  auto baseColorInput = shader->addInput("base_color", "color3");
  baseColorInput->setValue(toColor(pbr.baseColorFactor, defaultColor));

  const auto baseColorTextureIndex = pbr.baseColorTexture.index;
  if (baseColorTextureIndex >= 0) {
    auto imageNode = document->addNode("image",
                                       (materialName.empty() ? "baseColor_tex"
                                                            : materialName + "_baseColor_tex"),
                                       "color3");
    auto fileInput = imageNode->addInput("file", "filename");
    auto textureUri = resolveTextureUri(model, baseColorTextureIndex);
    if (!textureUri.empty()) {
      fileInput->setValueString(textureUri);
    }
    baseColorInput->setConnectedNode(imageNode);
  }

  auto metallicInput = shader->addInput("metalness", "float");
  metallicInput->setValue(static_cast<float>(pbr.metallicFactor));

  auto roughnessInput = shader->addInput("specular_roughness", "float");
  roughnessInput->setValue(static_cast<float>(pbr.roughnessFactor));

  const auto metallicRoughnessTextureIndex = pbr.metallicRoughnessTexture.index;
  if (metallicRoughnessTextureIndex >= 0) {
    auto imageNode = document->addNode(
        "image",
        (materialName.empty() ? "metalRough_tex" : materialName + "_metalRough_tex"),
        "color3");
    auto fileInput = imageNode->addInput("file", "filename");
    auto textureUri = resolveTextureUri(model, metallicRoughnessTextureIndex);
    if (!textureUri.empty()) {
      fileInput->setValueString(textureUri);
    }

    auto roughnessExtract = document->addNode(
        "extract",
        (materialName.empty() ? "roughness_extract"
                              : materialName + "_roughness_extract"),
        "float");
    roughnessExtract->addInput("in", "color3")->setConnectedNode(imageNode);
    roughnessExtract->addInput("index", "integer")->setValue(1);
    roughnessInput->setConnectedNode(roughnessExtract);

    auto metallicExtract = document->addNode(
        "extract",
        (materialName.empty() ? "metallic_extract"
                              : materialName + "_metallic_extract"),
        "float");
    metallicExtract->addInput("in", "color3")->setConnectedNode(imageNode);
    metallicExtract->addInput("index", "integer")->setValue(2);
    metallicInput->setConnectedNode(metallicExtract);
  }

  const auto normalTextureIndex = gltfMaterial.normalTexture.index;
  if (normalTextureIndex >= 0) {
    auto normalImage = document->addNode(
        "image",
        (materialName.empty() ? "normal_tex" : materialName + "_normal_tex"),
        "vector3");
    auto fileInput = normalImage->addInput("file", "filename");
    auto textureUri = resolveTextureUri(model, normalTextureIndex);
    if (!textureUri.empty()) {
      fileInput->setValueString(textureUri);
    }

    auto normalMap = document->addNode(
        "normalmap",
        (materialName.empty() ? "normalmap" : materialName + "_normalmap"),
        "vector3");
    normalMap->addInput("in", "vector3")->setConnectedNode(normalImage);
    float normalScale = static_cast<float>(gltfMaterial.normalTexture.scale);
    normalMap->addInput("scale", "float")->setValue(normalScale == 0.0f ? 1.0f : normalScale);

    auto normalInput = shader->addInput("normal", "vector3");
    normalInput->setConnectedNode(normalMap);
  }

  const auto occlusionTextureIndex = gltfMaterial.occlusionTexture.index;
  if (occlusionTextureIndex >= 0) {
    auto occlusionImage = document->addNode(
        "image",
        (materialName.empty() ? "occlusion_tex" : materialName + "_occlusion_tex"),
        "color3");
    auto fileInput = occlusionImage->addInput("file", "filename");
    auto textureUri = resolveTextureUri(model, occlusionTextureIndex);
    if (!textureUri.empty()) {
      fileInput->setValueString(textureUri);
    }

    auto occlusionExtract = document->addNode(
        "extract",
        (materialName.empty() ? "occlusion_extract"
                              : materialName + "_occlusion_extract"),
        "float");
    occlusionExtract->addInput("in", "color3")->setConnectedNode(occlusionImage);
    occlusionExtract->addInput("index", "integer")->setValue(0);

    auto occlusionInput = shader->addInput("occlusion", "float");
    float strength = static_cast<float>(gltfMaterial.occlusionTexture.strength);
    if (strength == 0.0f) {
      strength = 1.0f;
    }
    auto strengthMultiply = document->addNode(
        "multiply",
        (materialName.empty() ? "occlusion_strength"
                              : materialName + "_occlusion_strength"),
        "float");
    strengthMultiply->addInput("in1", "float")->setConnectedNode(occlusionExtract);
    strengthMultiply->addInput("in2", "float")->setValue(strength);
    occlusionInput->setConnectedNode(strengthMultiply);
  }

  auto emissionColorInput = shader->addInput("emission_color", "color3");
  const auto emissiveColor = toColor(gltfMaterial.emissiveFactor, blackColor);
  emissionColorInput->setValue(emissiveColor);

  const auto emissiveTextureIndex = gltfMaterial.emissiveTexture.index;
  if (emissiveTextureIndex >= 0) {
    auto emissiveImage = document->addNode(
        "image",
        (materialName.empty() ? "emissive_tex" : materialName + "_emissive_tex"),
        "color3");
    auto fileInput = emissiveImage->addInput("file", "filename");
    auto textureUri = resolveTextureUri(model, emissiveTextureIndex);
    if (!textureUri.empty()) {
      fileInput->setValueString(textureUri);
    }

    auto emissiveMultiply = document->addNode(
        "multiply",
        (materialName.empty() ? "emissive_mix" : materialName + "_emissive_mix"),
        "color3");
    emissiveMultiply->addInput("in1", "color3")->setConnectedNode(emissiveImage);
    emissiveMultiply->addInput("in2", "color3")->setValue(emissiveColor);
    emissionColorInput->setConnectedNode(emissiveMultiply);
  }

  auto shaderOutput = document->addOutput(
      materialName.empty() ? "out" : materialName + "_out", "surfaceshader");
  shaderOutput->setConnectedNode(shader);

  auto materialNode =
      document->addNode("material",
                        materialName.empty() ? "gltf_material" : materialName,
                        "material");
  auto surfaceShaderInput = materialNode->addInput("surfaceshader", "surfaceshader");
  surfaceShaderInput->setConnectedNode(shader);

  return document;
}

std::vector<MaterialX::DocumentPtr> SlangMaterialXBridge::loadGltfMaterials(
    const std::string& filename) const {
  tinygltf::TinyGLTF loader;
  tinygltf::Model model;
  std::string err;
  std::string warn;

  bool loaded = false;
  if (isBinaryGltf(filename)) {
    loaded = loader.LoadBinaryFromFile(&model, &err, &warn, filename);
  } else {
    loaded = loader.LoadASCIIFromFile(&model, &err, &warn, filename);
  }

  if (!warn.empty()) {
    std::clog << "glTF warning: " << warn << std::endl;
  }
  if (!loaded) {
    throw std::runtime_error("Failed to load glTF file: " + err);
  }

  return loadGltfMaterials(model);
}

std::vector<MaterialX::DocumentPtr> SlangMaterialXBridge::loadGltfMaterials(
    const tinygltf::Model& model) const {
  std::vector<MaterialX::DocumentPtr> documents;
  documents.reserve(model.materials.size());

  for (size_t i = 0; i < model.materials.size(); ++i) {
    const auto& gltfMaterial = model.materials[i];
    std::string name = gltfMaterial.name.empty()
                           ? "material_" + std::to_string(i)
                           : gltfMaterial.name;
    documents.push_back(createDocumentFromGltfMaterial(model, gltfMaterial, name));
  }

  return documents;
}

std::vector<uint32_t> SlangMaterialXBridge::loadTexturesForGltf(
    const tinygltf::Model& model, const std::filesystem::path& baseDir,
    utility::material::TextureManager& textureManager,
    const std::function<utility::material::TextureResource(
        const std::string&)>& textureLoader) const {
  std::vector<uint32_t> imageToTexture(model.images.size(),
                                       std::numeric_limits<uint32_t>::max());

  for (size_t i = 0; i < model.images.size(); ++i) {
    const auto& image = model.images[i];
    if (image.uri.empty()) continue;

    const auto fullPath = (baseDir / image.uri).lexically_normal().string();

    if (const auto cachedIndex = textureManager.findTextureIndex(fullPath)) {
      imageToTexture[i] = *cachedIndex;
      continue;
    }

    try {
      auto resource = textureLoader(fullPath);
      imageToTexture[i] = textureManager.registerTexture(resource);
    } catch (const std::exception& exc) {
      std::cerr << "Texture load failed for " << fullPath << ": "
                << exc.what() << std::endl;
    }
  }

  return imageToTexture;
}

void SlangMaterialXBridge::loadMaterialsForGltf(
    const tinygltf::Model& model, const std::vector<uint32_t>& imageToTexture,
    utility::material::MaterialManager& materialManager,
    uint32_t& defaultMaterialIndex) const {
  auto materialDocs = loadGltfMaterials(model);

  const auto resolveTextureIndexFromUri =
      [&](const std::string& uri) -> uint32_t {
    if (uri.empty()) {
      return std::numeric_limits<uint32_t>::max();
    }

    for (size_t i = 0; i < model.images.size(); ++i) {
      const auto& image = model.images[i];
      const auto fallbackName = "texture_" + std::to_string(i);
      if (uri == image.uri || uri == image.name || uri == fallbackName) {
        if (i < imageToTexture.size()) {
          return imageToTexture[i];
        }
      }
    }

    return std::numeric_limits<uint32_t>::max();
  };

  const auto existingMaterials = materialManager.materialCount();
  for (size_t i = 0; i < model.materials.size(); ++i) {
    const auto& mat = model.materials[i];
    utility::material::Material material{};
    if (mat.pbrMetallicRoughness.baseColorFactor.size() == 4) {
      material.baseColor =
          glm::vec4(static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[0]),
                    static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[1]),
                    static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[2]),
                    static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[3]));
    }

    material.metallicFactor =
        static_cast<float>(mat.pbrMetallicRoughness.metallicFactor);
    material.roughnessFactor =
        static_cast<float>(mat.pbrMetallicRoughness.roughnessFactor);
    if (mat.emissiveFactor.size() == 3) {
      material.emissiveColor = glm::vec3(
          static_cast<float>(mat.emissiveFactor[0]),
          static_cast<float>(mat.emissiveFactor[1]),
          static_cast<float>(mat.emissiveFactor[2]));
    }

    MaterialX::DocumentPtr materialDoc =
        i < materialDocs.size() ? materialDocs[i] : nullptr;

    if (materialDoc) {
      material.baseColor = extractBaseColor(materialDoc);
      material.emissiveColor =
          extractColorInput(materialDoc, "emission_color", material.emissiveColor);
      material.metallicFactor =
          extractFloatInput(materialDoc, "metalness", material.metallicFactor);
      material.roughnessFactor = extractFloatInput(materialDoc, "specular_roughness",
                                                  material.roughnessFactor);

      auto applyTextureFromDoc = [&](const std::string& inputName,
                                     uint32_t& destination) {
        auto uri = extractTextureFileForInput(materialDoc, inputName);
        auto resolved = resolveTextureIndexFromUri(uri);
        if (resolved != std::numeric_limits<uint32_t>::max()) {
          destination = resolved;
        }
      };

      applyTextureFromDoc("base_color", material.baseColorTextureIndex);
      applyTextureFromDoc("normal", material.normalTextureIndex);
      applyTextureFromDoc("occlusion", material.occlusionTextureIndex);
      applyTextureFromDoc("emission_color", material.emissiveTextureIndex);

      auto metallicUri = extractTextureFileForInput(materialDoc, "metalness");
      auto roughnessUri =
          extractTextureFileForInput(materialDoc, "specular_roughness");
      auto metallicResolved = resolveTextureIndexFromUri(metallicUri);
      auto roughnessResolved = resolveTextureIndexFromUri(roughnessUri);
      if (metallicResolved != std::numeric_limits<uint32_t>::max()) {
        material.metallicRoughnessTextureIndex = metallicResolved;
      } else if (roughnessResolved != std::numeric_limits<uint32_t>::max()) {
        material.metallicRoughnessTextureIndex = roughnessResolved;
      }
    }

    const auto baseIndex = mat.pbrMetallicRoughness.baseColorTexture.index;
    if (material.baseColorTextureIndex == std::numeric_limits<uint32_t>::max() &&
        baseIndex >= 0 &&
        static_cast<size_t>(baseIndex) < model.textures.size()) {
      const auto& tex = model.textures[baseIndex];
      if (tex.source >= 0 && static_cast<size_t>(tex.source) < imageToTexture.size()) {
        material.baseColorTextureIndex = imageToTexture[tex.source];
      }
    }

    const auto metallicIndex = mat.pbrMetallicRoughness.metallicRoughnessTexture.index;
    if (material.metallicRoughnessTextureIndex ==
            std::numeric_limits<uint32_t>::max() &&
        metallicIndex >= 0 && static_cast<size_t>(metallicIndex) < model.textures.size()) {
      const auto& tex = model.textures[metallicIndex];
      if (tex.source >= 0 && static_cast<size_t>(tex.source) < imageToTexture.size()) {
        material.metallicRoughnessTextureIndex = imageToTexture[tex.source];
      }
    }

    const auto normalIndex = mat.normalTexture.index;
    if (material.normalTextureIndex == std::numeric_limits<uint32_t>::max() &&
        normalIndex >= 0 && static_cast<size_t>(normalIndex) < model.textures.size()) {
      const auto& tex = model.textures[normalIndex];
      if (tex.source >= 0 && static_cast<size_t>(tex.source) < imageToTexture.size()) {
        material.normalTextureIndex = imageToTexture[tex.source];
      }
    }

    const auto occlusionIndex = mat.occlusionTexture.index;
    if (material.occlusionTextureIndex == std::numeric_limits<uint32_t>::max() &&
        occlusionIndex >= 0 &&
        static_cast<size_t>(occlusionIndex) < model.textures.size()) {
      const auto& tex = model.textures[occlusionIndex];
      if (tex.source >= 0 && static_cast<size_t>(tex.source) < imageToTexture.size()) {
        material.occlusionTextureIndex = imageToTexture[tex.source];
      }
    }

    const auto emissiveIndex = mat.emissiveTexture.index;
    if (material.emissiveTextureIndex == std::numeric_limits<uint32_t>::max() &&
        emissiveIndex >= 0 && static_cast<size_t>(emissiveIndex) < model.textures.size()) {
      const auto& tex = model.textures[emissiveIndex];
      if (tex.source >= 0 && static_cast<size_t>(tex.source) < imageToTexture.size()) {
        material.emissiveTextureIndex = imageToTexture[tex.source];
      }
    }

    materialManager.createMaterial(material);
  }

  if (!model.materials.empty()) {
    defaultMaterialIndex = static_cast<uint32_t>(existingMaterials);
  }
}

}  // namespace utility::materialx
