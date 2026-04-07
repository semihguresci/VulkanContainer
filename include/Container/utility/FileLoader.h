#pragma once

#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>
#include <vector>

namespace utility::file {

/**
 * @brief Read a file fully into a byte buffer.
 *
 * @param path Path to file
 * @return std::vector<char> File contents
 *
 * @throws std::runtime_error on failure
 */
[[nodiscard]] inline std::vector<char> readFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::ate | std::ios::binary);

  if (!file.is_open()) {
    throw std::runtime_error("Failed to open file: " + path.string());
  }

  const std::streamsize fileSize = file.tellg();
  if (fileSize <= 0) {
    throw std::runtime_error("File is empty or unreadable: " + path.string());
  }

  std::vector<char> buffer(static_cast<size_t>(fileSize));

  file.seekg(0, std::ios::beg);
  file.read(buffer.data(), fileSize);

  if (!file) {
    throw std::runtime_error("Failed to read file: " + path.string());
  }

  return buffer;
}

/**
 * @brief Read a file fully into a byte buffer without throwing.
 *
 * @param path Path to file
 * @return std::expected containing file contents on success, or an error
 *         message string on failure
 */
[[nodiscard]] inline std::expected<std::vector<char>, std::string> tryReadFile(
    const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::ate | std::ios::binary);

  if (!file.is_open()) {
    return std::unexpected("Failed to open file: " + path.string());
  }

  const std::streamsize fileSize = file.tellg();
  if (fileSize <= 0) {
    return std::unexpected("File is empty or unreadable: " + path.string());
  }

  std::vector<char> buffer(static_cast<size_t>(fileSize));

  file.seekg(0, std::ios::beg);
  file.read(buffer.data(), fileSize);

  if (!file) {
    return std::unexpected("Failed to read file: " + path.string());
  }

  return buffer;
}

}  // namespace utility::file
