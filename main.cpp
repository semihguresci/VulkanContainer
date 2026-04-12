
#include "Container/app/Application.h"
#include "Container/app/AppConfig.h"

#include <cstdlib>
#include <cstring>
#include <print>
#include <stdexcept>
#include <utility>

int main(int argc, char** argv) {
  try {
    auto config = container::app::DefaultAppConfig();
    if (argc > 1 && argv[1] != nullptr && std::strlen(argv[1]) > 0) {
      config.modelPath = argv[1];
    }
    container::app::Application application{std::move(config)};
    application.run();
  } catch (const std::exception& e) {
    std::println(stderr, "{}", e.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

