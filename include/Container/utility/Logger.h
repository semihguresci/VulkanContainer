#pragma once

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

#include "Container/common/CommonVulkan.h"  

namespace container::log {

using SpdLogger = std::shared_ptr<spdlog::logger>;
using SpdSink = std::shared_ptr<spdlog::sinks::sink>;

class ContainerLogger {
 public:
  ContainerLogger(const ContainerLogger&) = delete;
  ContainerLogger(ContainerLogger&&) = delete;
  ContainerLogger& operator=(const ContainerLogger&) = delete;
  ContainerLogger& operator=(ContainerLogger&&) = delete;

  static ContainerLogger& instance();

  SpdLogger& renderer();
  SpdLogger& vulkan();

  // Vulkan result logging with source location (C Vulkan)
  void log_vk_result(
      VkResult result, std::string_view operation,
      const std::source_location& location = std::source_location::current());

 private:
  ContainerLogger();
  ~ContainerLogger();

  SpdLogger renderer_logger_;
  SpdLogger vulkan_logger_;
};

// Helper function for Vulkan result checking
inline void check_vk_result(
    VkResult result, std::string_view operation,
    const std::source_location& location = std::source_location::current()) {
  ContainerLogger::instance().log_vk_result(result, operation, location);
}

}  // namespace container::log

