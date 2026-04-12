#pragma once

#include <filesystem>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace container::util {

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
