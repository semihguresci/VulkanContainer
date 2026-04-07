#include <gtest/gtest.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

class VulkanLoader {
 public:
  VulkanLoader() {
#if defined(_WIN32)
    handle_ = LoadLibraryA("vulkan-1.dll");
#else
    handle_ = dlopen("libvulkan.so.1", RTLD_LAZY);
    if (!handle_) {
      handle_ = dlopen("libvulkan.so", RTLD_LAZY);
    }
#endif
  }

  ~VulkanLoader() {
#if defined(_WIN32)
    if (handle_) {
      FreeLibrary(handle_);
    }
#else
    if (handle_) {
      dlclose(handle_);
    }
#endif
  }

  VulkanLoader(const VulkanLoader&) = delete;
  VulkanLoader& operator=(const VulkanLoader&) = delete;

  bool loaded() const { return handle_ != nullptr; }

  void* symbol(const char* name) const {
    if (!loaded()) {
      return nullptr;
    }
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(handle_, name));
#else
    return dlsym(handle_, name);
#endif
  }

 private:
#if defined(_WIN32)
  HMODULE handle_{nullptr};
#else
  void* handle_{nullptr};
#endif
};

}  // namespace

TEST(VulkanTest, LoaderLibraryIsAvailable) {
  VulkanLoader loader{};
  ASSERT_TRUE(loader.loaded());
}

TEST(VulkanTest, LoaderExportsCoreEntryPoint) {
  VulkanLoader loader{};
  ASSERT_TRUE(loader.loaded());
  ASSERT_NE(loader.symbol("vkGetInstanceProcAddr"), nullptr);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
