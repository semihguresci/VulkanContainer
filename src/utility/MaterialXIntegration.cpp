#include <Container/utility/MaterialXIntegration.h>
#include <MaterialXCore/Material.h>
#include <MaterialXCore/Util.h>
#include <MaterialXCore/Value.h>

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

}  // namespace utility::materialx
