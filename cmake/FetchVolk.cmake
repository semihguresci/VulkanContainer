FetchContent_Declare(
    volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG 1.3.295 
    SOURCE_DIR ${EXTERNAL_DIR}/volk-src
    BINARY_DIR ${EXTERNAL_DIR}/volk-build
)

FetchContent_MakeAvailable(volk)


