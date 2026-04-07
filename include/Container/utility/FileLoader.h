#pragma once

#include <expected>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

namespace utility::file {

/**
 * @brief Read a file fully into a byte buffer.
 *
 * @param filename Path to file
 * @return std::expected<std::vector<char>, std::string> File contents on
 *         success, or an error message on failure.
 */
[[nodiscard]] inline std::expected<std::vector<char>, std::string> readFile(
    std::string_view filename) {
  std::ifstream file(std::string(filename), std::ios::ate | std::ios::binary);

  if (!file.is_open()) {
    return std::unexpected("Failed to open file: " + std::string(filename));
  }

  const std::streamsize fileSize = file.tellg();
  if (fileSize <= 0) {
    return std::unexpected("File is empty or unreadable: " +
                           std::string(filename));
  }

  std::vector<char> buffer(static_cast<size_t>(fileSize));

  file.seekg(0, std::ios::beg);
  file.read(buffer.data(), fileSize);

  if (!file) {
    return std::unexpected("Failed to read file: " + std::string(filename));
  }

  return buffer;
}

}  // namespace utility::file
