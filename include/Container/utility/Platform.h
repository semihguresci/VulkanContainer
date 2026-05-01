#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace container::util {

[[nodiscard]] inline std::string pathToUtf8(const std::filesystem::path& path) {
  const auto value = path.u8string();
  std::string result;
  result.reserve(value.size());
  for (const auto ch : value) {
    result.push_back(static_cast<char>(ch));
  }
  return result;
}

[[nodiscard]] inline std::filesystem::path pathFromUtf8(std::string_view value) {
#ifdef __cpp_char8_t
  std::u8string utf8;
  utf8.reserve(value.size());
  for (const char ch : value) {
    utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(ch)));
  }
  return std::filesystem::path(utf8);
#else
  return std::filesystem::u8path(value);
#endif
}

// Returns the directory containing the running executable.
// Falls back to the current working directory on error.
[[nodiscard]] inline std::filesystem::path executableDirectory() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH]{};
  const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (len > 0 && len < MAX_PATH) {
    return std::filesystem::path(buf).parent_path();
  }
#else
  std::error_code ec;
  auto p = std::filesystem::canonical("/proc/self/exe", ec);
  if (!ec) return p.parent_path();
#endif
  return std::filesystem::current_path();
}

}  // namespace container::util
