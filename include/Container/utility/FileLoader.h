#pragma once

#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>
#include <vector>

namespace utility::file {

/**
 * @brief Read a file fully into a byte buffer.
 *
 * @param filename Path to file
 * @return std::vector<char> File contents
 *
 * @throws std::runtime_error on failure
 */
inline std::vector<char> readFile(const std::string& filename) {
  std::ifstream file(filename, std::ios::ate | std::ios::binary);

  if (!file.is_open()) {
    throw std::runtime_error("Failed to open file: " + filename);
  }

  const std::streamsize fileSize = file.tellg();
  if (fileSize <= 0) {
    throw std::runtime_error("File is empty or unreadable: " + filename);
  }

  std::vector<char> buffer(static_cast<size_t>(fileSize));

  file.seekg(0, std::ios::beg);
  file.read(buffer.data(), fileSize);

  if (!file) {
    throw std::runtime_error("Failed to read file: " + filename);
  }

  return buffer;
}

}  // namespace utility::file
