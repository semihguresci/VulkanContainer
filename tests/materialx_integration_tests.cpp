#include "Container/utility/MaterialManager.h"
#include "Container/utility/MaterialXIntegration.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <limits>
#include <vector>

namespace {

tinygltf::Value numberArray(std::initializer_list<double> values) {
  tinygltf::Value::Array array;
  array.reserve(values.size());
  for (double value : values) {
    array.emplace_back(value);
  }
  return tinygltf::Value(std::move(array));
}

tinygltf::Value textureInfo(int index) {
  tinygltf::Value::Object object;
  object.emplace("index", tinygltf::Value(index));
  return tinygltf::Value(std::move(object));
}

tinygltf::Value textureInfoWithScale(int index, double scale) {
  tinygltf::Value::Object object;
  object.emplace("index", tinygltf::Value(index));
  object.emplace("scale", tinygltf::Value(scale));
  return tinygltf::Value(std::move(object));
}

}  // namespace

TEST(MaterialXIntegration, ImportsKhrMaterialsPbrSpecularGlossiness) {
  tinygltf::Model model;
  model.images.resize(2);
  model.textures.resize(2);
  model.textures[0].source = 0;
  model.textures[1].source = 1;

  tinygltf::Material gltfMaterial;
  gltfMaterial.alphaMode = "BLEND";

  tinygltf::Value::Object specGloss;
  specGloss.emplace("diffuseFactor",
                    numberArray({0.0, 0.0, 0.0, 0.6}));
  specGloss.emplace("specularFactor",
                    numberArray({1.0, 0.766, 0.336}));
  specGloss.emplace("glossinessFactor", tinygltf::Value(0.9));
  specGloss.emplace("diffuseTexture", textureInfo(0));
  specGloss.emplace("specularGlossinessTexture", textureInfo(1));
  gltfMaterial.extensions.emplace(
      "KHR_materials_pbrSpecularGlossiness",
      tinygltf::Value(std::move(specGloss)));
  model.materials.push_back(std::move(gltfMaterial));

  container::material::SlangMaterialXBridge bridge;
  container::material::MaterialManager materialManager;
  uint32_t defaultMaterialIndex = std::numeric_limits<uint32_t>::max();
  const std::vector<uint32_t> imageToTexture{7u, 11u};

  bridge.loadMaterialsForGltf(model, imageToTexture, materialManager,
                              defaultMaterialIndex);

  ASSERT_EQ(materialManager.materialCount(), 1u);
  const auto* material = materialManager.getMaterial(0);
  ASSERT_NE(material, nullptr);

  EXPECT_TRUE(material->specularGlossinessWorkflow);
  EXPECT_EQ(material->baseColorTextureIndex, 7u);
  EXPECT_EQ(material->roughnessTextureIndex, 11u);
  EXPECT_EQ(material->specularColorTextureIndex, 11u);
  EXPECT_EQ(material->specularTextureIndex,
            std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(material->metallicRoughnessTextureIndex,
            std::numeric_limits<uint32_t>::max());
  EXPECT_NEAR(material->baseColor.r, 1.0f, 1e-4f);
  EXPECT_NEAR(material->baseColor.g, 0.766f, 1e-4f);
  EXPECT_NEAR(material->baseColor.b, 0.336f, 1e-4f);
  EXPECT_NEAR(material->baseColor.a, 0.6f, 1e-4f);
  EXPECT_NEAR(material->metallicFactor, 1.0f, 1e-4f);
  EXPECT_NEAR(material->roughnessFactor, 0.9f, 1e-4f);
  EXPECT_EQ(material->alphaMode, container::material::AlphaMode::Blend);
  EXPECT_EQ(defaultMaterialIndex, 0u);
}

TEST(MaterialXIntegration, ImportsLayeredKhrPbrExtensions) {
  tinygltf::Model model;
  model.images.resize(11);
  model.textures.resize(11);
  for (size_t i = 0; i < model.textures.size(); ++i) {
    model.textures[i].source = static_cast<int>(i);
  }

  tinygltf::Material gltfMaterial;
  gltfMaterial.alphaMode = "OPAQUE";
  gltfMaterial.emissiveFactor = {0.25, 0.5, 0.75};

  tinygltf::Value::Object specular;
  specular.emplace("specularFactor", tinygltf::Value(0.5));
  specular.emplace("specularColorFactor",
                   numberArray({0.8, 0.7, 0.6}));
  specular.emplace("specularTexture", textureInfo(0));
  specular.emplace("specularColorTexture", textureInfo(1));
  gltfMaterial.extensions.emplace("KHR_materials_specular",
                                  tinygltf::Value(std::move(specular)));

  tinygltf::Value::Object clearcoat;
  clearcoat.emplace("clearcoatFactor", tinygltf::Value(0.7));
  clearcoat.emplace("clearcoatTexture", textureInfo(2));
  clearcoat.emplace("clearcoatRoughnessFactor", tinygltf::Value(0.2));
  clearcoat.emplace("clearcoatRoughnessTexture", textureInfo(3));
  clearcoat.emplace("clearcoatNormalTexture", textureInfoWithScale(4, 0.4));
  gltfMaterial.extensions.emplace("KHR_materials_clearcoat",
                                  tinygltf::Value(std::move(clearcoat)));

  tinygltf::Value::Object transmission;
  transmission.emplace("transmissionFactor", tinygltf::Value(0.6));
  transmission.emplace("transmissionTexture", textureInfo(5));
  gltfMaterial.extensions.emplace("KHR_materials_transmission",
                                  tinygltf::Value(std::move(transmission)));

  tinygltf::Value::Object volume;
  volume.emplace("thicknessFactor", tinygltf::Value(0.2));
  volume.emplace("thicknessTexture", textureInfo(6));
  volume.emplace("attenuationColor", numberArray({0.1, 0.2, 0.3}));
  volume.emplace("attenuationDistance", tinygltf::Value(4.0));
  gltfMaterial.extensions.emplace("KHR_materials_volume",
                                  tinygltf::Value(std::move(volume)));

  tinygltf::Value::Object sheen;
  sheen.emplace("sheenColorFactor", numberArray({0.3, 0.2, 0.1}));
  sheen.emplace("sheenColorTexture", textureInfo(7));
  sheen.emplace("sheenRoughnessFactor", tinygltf::Value(0.55));
  sheen.emplace("sheenRoughnessTexture", textureInfo(8));
  gltfMaterial.extensions.emplace("KHR_materials_sheen",
                                  tinygltf::Value(std::move(sheen)));

  tinygltf::Value::Object iridescence;
  iridescence.emplace("iridescenceFactor", tinygltf::Value(0.8));
  iridescence.emplace("iridescenceTexture", textureInfo(9));
  iridescence.emplace("iridescenceIor", tinygltf::Value(1.4));
  iridescence.emplace("iridescenceThicknessMinimum", tinygltf::Value(120.0));
  iridescence.emplace("iridescenceThicknessMaximum", tinygltf::Value(450.0));
  iridescence.emplace("iridescenceThicknessTexture", textureInfo(10));
  gltfMaterial.extensions.emplace("KHR_materials_iridescence",
                                  tinygltf::Value(std::move(iridescence)));

  tinygltf::Value::Object emissiveStrength;
  emissiveStrength.emplace("emissiveStrength", tinygltf::Value(2.5));
  gltfMaterial.extensions.emplace("KHR_materials_emissive_strength",
                                  tinygltf::Value(std::move(emissiveStrength)));

  tinygltf::Value::Object ior;
  ior.emplace("ior", tinygltf::Value(1.33));
  gltfMaterial.extensions.emplace("KHR_materials_ior",
                                  tinygltf::Value(std::move(ior)));

  tinygltf::Value::Object dispersion;
  dispersion.emplace("dispersion", tinygltf::Value(15.0));
  gltfMaterial.extensions.emplace("KHR_materials_dispersion",
                                  tinygltf::Value(std::move(dispersion)));

  gltfMaterial.extensions.emplace(
      "KHR_materials_unlit",
      tinygltf::Value(tinygltf::Value::Object{}));
  model.materials.push_back(std::move(gltfMaterial));

  container::material::SlangMaterialXBridge bridge;
  container::material::MaterialManager materialManager;
  uint32_t defaultMaterialIndex = std::numeric_limits<uint32_t>::max();
  const std::vector<uint32_t> imageToTexture{
      20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u};

  bridge.loadMaterialsForGltf(model, imageToTexture, materialManager,
                              defaultMaterialIndex);

  ASSERT_EQ(materialManager.materialCount(), 1u);
  const auto* material = materialManager.getMaterial(0);
  ASSERT_NE(material, nullptr);

  EXPECT_NEAR(material->specularFactor, 0.5f, 1e-4f);
  EXPECT_NEAR(material->specularColorFactor.r, 0.8f, 1e-4f);
  EXPECT_EQ(material->specularTextureIndex, 20u);
  EXPECT_EQ(material->specularColorTextureIndex, 21u);
  EXPECT_NEAR(material->clearcoatFactor, 0.7f, 1e-4f);
  EXPECT_NEAR(material->clearcoatRoughnessFactor, 0.2f, 1e-4f);
  EXPECT_NEAR(material->clearcoatNormalTextureScale, 0.4f, 1e-4f);
  EXPECT_EQ(material->clearcoatTextureIndex, 22u);
  EXPECT_EQ(material->clearcoatRoughnessTextureIndex, 23u);
  EXPECT_EQ(material->clearcoatNormalTextureIndex, 24u);
  EXPECT_NEAR(material->transmissionFactor, 0.6f, 1e-4f);
  EXPECT_EQ(material->transmissionTextureIndex, 25u);
  EXPECT_NEAR(material->thicknessFactor, 0.2f, 1e-4f);
  EXPECT_EQ(material->thicknessTextureIndex, 26u);
  EXPECT_NEAR(material->attenuationColor.g, 0.2f, 1e-4f);
  EXPECT_NEAR(material->attenuationDistance, 4.0f, 1e-4f);
  EXPECT_NEAR(material->sheenColorFactor.r, 0.3f, 1e-4f);
  EXPECT_NEAR(material->sheenRoughnessFactor, 0.55f, 1e-4f);
  EXPECT_EQ(material->sheenColorTextureIndex, 27u);
  EXPECT_EQ(material->sheenRoughnessTextureIndex, 28u);
  EXPECT_NEAR(material->iridescenceFactor, 0.8f, 1e-4f);
  EXPECT_NEAR(material->iridescenceIor, 1.4f, 1e-4f);
  EXPECT_NEAR(material->iridescenceThicknessMinimum, 120.0f, 1e-4f);
  EXPECT_NEAR(material->iridescenceThicknessMaximum, 450.0f, 1e-4f);
  EXPECT_EQ(material->iridescenceTextureIndex, 29u);
  EXPECT_EQ(material->iridescenceThicknessTextureIndex, 30u);
  EXPECT_NEAR(material->emissiveStrength, 2.5f, 1e-4f);
  EXPECT_NEAR(material->ior, 1.33f, 1e-4f);
  EXPECT_NEAR(material->dispersion, 15.0f, 1e-4f);
  EXPECT_TRUE(material->unlit);
  EXPECT_EQ(material->alphaMode, container::material::AlphaMode::Blend);
}
