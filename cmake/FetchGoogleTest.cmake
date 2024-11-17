include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    SOURCE_DIR ${EXTERNAL_DIR}/googletest-src
    BINARY_DIR ${EXTERNAL_DIR}/googletest-build
)

FetchContent_MakeAvailable(googletest)