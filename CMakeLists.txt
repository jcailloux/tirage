cmake_minimum_required(VERSION 3.20)
project(tirage VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TIRAGE_BUILD_TESTS "Build the doctest unit tests" ${PROJECT_IS_TOP_LEVEL})
option(TIRAGE_BUILD_TOOLS "Build tirage-worker (needs libvips), tiraged and the tirage CLI" ${PROJECT_IS_TOP_LEVEL})

set(TIRAGE_WARNINGS -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers)  # designated initializers leave optionals out on purpose

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
# tirage::profile: profile schema, protocol and their validation.
# Pure logic, header-only, no libvips: shared by the daemon, the worker and the
# client (which is header-only too, hence this library is). direct.h, the
# direct mode launcher, lives here too: POSIX only, still no libvips.
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
# tirage::client (libtirage-client): the caller's side of the daemon's socket,
# header-only too (include/tirage/client.h). A site links this one, with
# TIRAGE_BUILD_TOOLS off (the default for a FetchContent dependency).
# =============================================================================
add_library(tirage-client INTERFACE)
add_library(tirage::client ALIAS tirage-client)
target_link_libraries(tirage-client INTERFACE tirage::profile)

# =============================================================================
# Binaries. Only the worker links libvips: the daemon, the CLI and the client
# never decode an image.
# =============================================================================
if(TIRAGE_BUILD_TOOLS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VIPS REQUIRED IMPORTED_TARGET vips-cpp>=8.16)

    add_executable(tirage-worker src/worker/main.cpp)
    target_link_libraries(tirage-worker PRIVATE tirage::profile PkgConfig::VIPS)
    target_compile_definitions(tirage-worker PRIVATE TIRAGE_VERSION="${PROJECT_VERSION}")
    target_compile_options(tirage-worker PRIVATE ${TIRAGE_WARNINGS})

    add_executable(tiraged src/daemon/main.cpp)
    target_link_libraries(tiraged PRIVATE tirage::profile)
    target_compile_definitions(tiraged PRIVATE TIRAGE_VERSION="${PROJECT_VERSION}")
    target_compile_options(tiraged PRIVATE ${TIRAGE_WARNINGS})

    add_executable(tirage src/cli/main.cpp)
    target_link_libraries(tirage PRIVATE tirage::client)
    target_compile_definitions(tirage PRIVATE TIRAGE_VERSION="${PROJECT_VERSION}")
    target_compile_options(tirage PRIVATE ${TIRAGE_WARNINGS})
endif()

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
    # The daemon's pure logic (scheduler, configuration) is tested from src/.
    target_include_directories(tirage-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tirage-tests PRIVATE tirage::profile)
    target_compile_options(tirage-tests PRIVATE ${TIRAGE_WARNINGS})

    enable_testing()
    add_test(NAME tirage-tests COMMAND tirage-tests --duration=true)

    # Integration harness: runs the real worker through direct mode, then
    # through the daemon, the client and the CLI, on images it builds with
    # libvips, and decodes what came back.
    if(TIRAGE_BUILD_TOOLS)
        file(GLOB TIRAGE_INTEGRATION_SOURCES CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/*.cpp)
        add_executable(tirage-integration ${TIRAGE_INTEGRATION_SOURCES})
        target_include_directories(tirage-integration SYSTEM PRIVATE ${doctest_SOURCE_DIR})
        target_link_libraries(tirage-integration PRIVATE tirage::client PkgConfig::VIPS)
        target_compile_definitions(tirage-integration PRIVATE
            TIRAGE_WORKER_PATH="$<TARGET_FILE:tirage-worker>"
            TIRAGE_DAEMON_PATH="$<TARGET_FILE:tiraged>"
            TIRAGE_CLI_PATH="$<TARGET_FILE:tirage>"
            TIRAGE_VERSION="${PROJECT_VERSION}")
        target_compile_options(tirage-integration PRIVATE ${TIRAGE_WARNINGS})
        add_dependencies(tirage-integration tirage-worker tiraged tirage)
        add_test(NAME tirage-integration COMMAND tirage-integration --duration=true)
        set_tests_properties(tirage-integration PROPERTIES LABELS integration)
    endif()
endif()
