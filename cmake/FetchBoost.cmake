include(FetchContent)

set(BOOST_VERSION "1.85.0")
string(REPLACE "." "_" BOOST_VERSION_UNDERSCORE ${BOOST_VERSION})
set(BOOST_URL "https://boostorg.jfrog.io/artifactory/main/release/${BOOST_VERSION}/source/boost_${BOOST_VERSION_UNDERSCORE}.tar.gz")

FetchContent_Declare(
    Boost
    URL ${BOOST_URL}
    SOURCE_DIR ${EXTERNAL_DIR}/boost-src
    BINARY_DIR ${EXTERNAL_DIR}/boost-build
)
FetchContent_MakeAvailable(Boost)


