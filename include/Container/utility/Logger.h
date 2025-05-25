#ifndef UTILITY_LOGGER_H
#define UTILITY_LOGGER_H

#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

#include <vulkan/vulkan.h>  
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

using SpdLogger = std::shared_ptr<spdlog::logger>;
using SpdSink = std::shared_ptr<spdlog::sinks::sink>;

namespace utility {  
namespace logger {  

class ContainerLogger {  
 public:  
  // Delete copy/move operations for singleton  
  ContainerLogger(const ContainerLogger&) = delete;  
  ContainerLogger(ContainerLogger&&) = delete;  
  ContainerLogger& operator=(const ContainerLogger&) = delete;  
  ContainerLogger& operator=(ContainerLogger&&) = delete;  

  static ContainerLogger& instance();  

  // Logger accessors  
  SpdLogger& renderer();  
  SpdLogger& vulkan();  

  // Vulkan result logging with source location  
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

}  // namespace logger  
}  // namespace utility

#endif  // UTILITY_LOGGER_H