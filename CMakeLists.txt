cmake_minimum_required(VERSION 3.20)
project(tirage VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TIRAGE_BUILD_TESTS "Build the doctest unit tests" ${PROJECT_IS_TOP_LEVEL})

# =============================================================================
# Dependencies
# =============================================================================
include(FetchContent)

FetchContent_Declare(
    glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG        v7.8.1
    GIT_SHALLOW    TRUE
    FIND_PACKAGE_ARGS 7.8
)
FetchContent_MakeAvailable(glaze)

# =============================================================================
# tirage::profile: profile schema and validation.
# Pure logic, header-only, no libvips: shared by the daemon, the worker and the
# client (which is header-only too, hence this library is).
# =============================================================================
add_library(tirage-profile INTERFACE)
add_library(tirage::profile ALIAS tirage-profile)
target_include_directories(tirage-profile INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(tirage-profile INTERFACE cxx_std_23)
target_link_libraries(tirage-profile INTERFACE glaze::glaze)

# =============================================================================
# Tests
# =============================================================================
if(TIRAGE_BUILD_TESTS)
    # doctest is header-only: fetch it without configuring its CMakeLists, whose
    # cmake_minimum_required is too old for CMake >= 4 (same as codiga).
    FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.5.3
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  no-cmake-here
    )
    FetchContent_MakeAvailable(doctest)

    file(GLOB TIRAGE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
    add_executable(tirage-tests ${TIRAGE_TEST_SOURCES})
    target_include_directories(tirage-tests SYSTEM PRIVATE ${doctest_SOURCE_DIR})
    target_link_libraries(tirage-tests PRIVATE tirage::profile)
    target_compile_options(tirage-tests PRIVATE -Wall -Wextra -Wpedantic
        -Wno-missing-field-initializers)  # designated initializers leave optionals out on purpose

    enable_testing()
    add_test(NAME tirage-tests COMMAND tirage-tests --duration=true)
endif()
