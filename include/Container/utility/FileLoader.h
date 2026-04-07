#pragma once

#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

namespace utility::file {

/**
 * @brief Read a file fully into a byte buffer.
 *
 * @param path Path to file (accepts std::string, std::string_view, const
 *             char*, or std::filesystem::path)
 * @return std::expected<std::vector<char>, std::string> File contents on
 *         success, or an error message on failure.
 */
[[nodiscard]] inline std::expected<std::vector<char>, std::string> readFile(
    const std::filesystem::path& path) {
  std::ifstream file{path, std::ios::ate | std::ios::binary};

  if (!file.is_open()) {
    return std::unexpected(
        std::format("Failed to open file: {}", path.string()));
  }

  const std::streamsize fileSize = file.tellg();
  if (fileSize <= 0) {
    return std::unexpected(
        std::format("File is empty or unreadable: {}", path.string()));
  }

  std::vector<char> buffer(static_cast<size_t>(fileSize));

  file.seekg(0, std::ios::beg);
  file.read(buffer.data(), fileSize);

  if (!file) {
    return std::unexpected(
        std::format("Failed to read file: {}", path.string()));
  }

  return buffer;
}

}  // namespace utility::file
