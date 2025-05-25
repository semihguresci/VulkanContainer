#include <Container/utility/Logger.h>
#include <Container/utility/VkResultToString.h>

namespace utility {
namespace logger {

ContainerLogger& ContainerLogger::instance() {
  static ContainerLogger instance;
  return instance;
}

ContainerLogger::ContainerLogger() {
  // Create console sink
  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  console_sink->set_level(spdlog::level::trace);

  // Create file sink (example)
  auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
      "logs/container.log", true);
  file_sink->set_level(spdlog::level::trace);

  // Create loggers
  renderer_logger_ = std::make_shared<spdlog::logger>("Renderer", console_sink);
  vulkan_logger_ = std::make_shared<spdlog::logger>("Vulkan", console_sink);

  // Register loggers (optional)
  spdlog::register_logger(renderer_logger_);
  spdlog::register_logger(vulkan_logger_);

  // Set default logging level
  renderer_logger_->set_level(spdlog::level::trace);
  vulkan_logger_->set_level(spdlog::level::trace);
}

ContainerLogger::~ContainerLogger() {
  spdlog::drop_all();  // Cleanup
}

SpdLogger& ContainerLogger::renderer() { return renderer_logger_; }

SpdLogger& ContainerLogger::vulkan() { return vulkan_logger_; }

void ContainerLogger::log_vk_result(VkResult result, std::string_view operation,
                                    const std::source_location& location) {
  if (result != VK_SUCCESS) {
    vulkan_logger_->error("Vulkan error in {} ({}:{}): {}", operation,
                          location.file_name(), location.line(),
                          utility::vkResultToString(result));
  }
}

}  // namespace logger
}  // namespace utility