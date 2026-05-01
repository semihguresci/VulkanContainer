
#include "Container/app/Application.h"
#include "Container/app/AppConfig.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

std::string_view requireValue(int argc, char** argv, int& index,
                              std::string_view option) {
  if (index + 1 >= argc || argv[index + 1] == nullptr) {
    throw std::runtime_error("missing value for " + std::string(option));
  }
  ++index;
  return argv[index];
}

float parseFloat(std::string_view value, std::string_view option) {
  try {
    return std::stof(std::string(value));
  } catch (...) {
    throw std::runtime_error("invalid float for " + std::string(option) +
                             ": " + std::string(value));
  }
}

uint32_t parseUint(std::string_view value, std::string_view option) {
  try {
    return static_cast<uint32_t>(std::stoul(std::string(value)));
  } catch (...) {
    throw std::runtime_error("invalid integer for " + std::string(option) +
                             ": " + std::string(value));
  }
}

std::array<float, 3> parseVec3(int argc, char** argv, int& index,
                               std::string_view option) {
  std::array<float, 3> result{};
  for (float& channel : result) {
    channel = parseFloat(requireValue(argc, argv, index, option), option);
  }
  return result;
}

void applyCommandLine(container::app::AppConfig& config, int argc,
                      char** argv) {
  bool positionalModelConsumed = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i] ? std::string_view(argv[i]) : "";
    if (arg == "--model") {
      config.modelPath = std::string(requireValue(argc, argv, i, arg));
      positionalModelConsumed = true;
    } else if (arg == "--width") {
      config.windowWidth = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--height") {
      config.windowHeight = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--import-scale") {
      config.importScale = parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--visual-regression-capture" ||
               arg == "--screenshot") {
      config.screenshotCapturePath =
          std::string(requireValue(argc, argv, i, arg));
    } else if (arg == "--warmup-frames") {
      config.screenshotWarmupFrames =
          parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--capture-frame") {
      config.screenshotCaptureFrame =
          parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--fixed-dt") {
      config.screenshotFixedTimestepSeconds =
          parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--camera-position") {
      config.cameraPosition = parseVec3(argc, argv, i, arg);
      config.hasCameraOverride = true;
    } else if (arg == "--camera-target") {
      config.cameraTarget = parseVec3(argc, argv, i, arg);
      config.hasCameraOverride = true;
    } else if (arg == "--camera-fov") {
      config.cameraVerticalFovDegrees =
          parseFloat(requireValue(argc, argv, i, arg), arg);
      config.hasCameraOverride = true;
    } else if (arg == "--exposure") {
      config.manualExposure = parseFloat(requireValue(argc, argv, i, arg), arg);
      config.hasManualExposureOverride = true;
    } else if (arg == "--environment-intensity") {
      config.environmentIntensity =
          parseFloat(requireValue(argc, argv, i, arg), arg);
      config.hasEnvironmentIntensityOverride = true;
    } else if (arg == "--directional-intensity") {
      config.directionalIntensity =
          parseFloat(requireValue(argc, argv, i, arg), arg);
      config.hasDirectionalIntensityOverride = true;
    } else if (arg == "--validation") {
      config.enableValidationLayers = true;
    } else if (arg == "--no-validation") {
      config.enableValidationLayers = false;
    } else if (arg == "--no-ui") {
      config.enableGui = false;
    } else if (arg == "--hidden") {
      config.windowVisible = false;
    } else if (!arg.starts_with("--") && !positionalModelConsumed) {
      config.modelPath = std::string(arg);
      positionalModelConsumed = true;
    } else {
      throw std::runtime_error("unknown argument: " + std::string(arg));
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    auto config = container::app::DefaultAppConfig();
    applyCommandLine(config, argc, argv);
    container::app::Application application{std::move(config)};
    application.run();
  } catch (const std::exception& e) {
    std::println(stderr, "{}", e.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

