cmake_minimum_required(VERSION 3.20)
project(tirage VERSION 0.3.1 LANGUAGES CXX)

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
    include(GNUInstallDirs)
    # The worker sits apart from the commands: nobody runs it by hand.
    set(TIRAGE_WORKER_DIR ${CMAKE_INSTALL_LIBEXECDIR}/tirage)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VIPS REQUIRED IMPORTED_TARGET vips-cpp>=8.16)

    add_executable(tirage-worker src/worker/main.cpp)
    target_link_libraries(tirage-worker PRIVATE tirage::profile PkgConfig::VIPS)
    target_compile_definitions(tirage-worker PRIVATE TIRAGE_VERSION="${PROJECT_VERSION}")
    target_compile_options(tirage-worker PRIVATE ${TIRAGE_WARNINGS})

    add_executable(tiraged src/daemon/main.cpp)
    target_link_libraries(tiraged PRIVATE tirage::profile)
    target_compile_definitions(tiraged PRIVATE TIRAGE_VERSION="${PROJECT_VERSION}"
        TIRAGE_DEFAULT_WORKER="${CMAKE_INSTALL_FULL_LIBEXECDIR}/tirage/tirage-worker")
    target_compile_options(tiraged PRIVATE ${TIRAGE_WARNINGS})

    add_executable(tirage src/cli/main.cpp)
    target_link_libraries(tirage PRIVATE tirage::client)
    target_compile_definitions(tirage PRIVATE TIRAGE_VERSION="${PROJECT_VERSION}")
    target_compile_options(tirage PRIVATE ${TIRAGE_WARNINGS})

    # =========================================================================
    # Installation and the Debian package (plan § 8): cmake --install for any
    # distribution, cpack -G DEB for trixie (packaging/build.sh builds it there).
    # Units and sysusers go under the prefix's lib/, where systemd looks for
    # them when the prefix is /usr.
    # =========================================================================
    set(TIRAGE_SYSTEMD_UNIT_DIR lib/systemd/system CACHE STRING "systemd units, relative to the prefix")
    set(TIRAGE_SYSUSERS_DIR lib/sysusers.d CACHE STRING "sysusers.d files, relative to the prefix")

    install(TARGETS tiraged DESTINATION ${CMAKE_INSTALL_SBINDIR})
    install(TARGETS tirage DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(TARGETS tirage-worker DESTINATION ${TIRAGE_WORKER_DIR})

    configure_file(packaging/systemd/tirage.service.in tirage.service @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/tirage.service
                  packaging/systemd/tirage.socket
                  packaging/systemd/tirage.slice
            DESTINATION ${TIRAGE_SYSTEMD_UNIT_DIR})
    install(FILES packaging/sysusers.d/tirage.conf DESTINATION ${TIRAGE_SYSUSERS_DIR})
    install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
    install(DIRECTORY docs examples DESTINATION ${CMAKE_INSTALL_DOCDIR})

    set(CPACK_PACKAGE_NAME tirage)
    set(CPACK_PACKAGE_VENDOR jcailloux)
    set(CPACK_PACKAGE_CONTACT "jocelin <64426530+jocelin-cailloux@users.noreply.github.com>")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Machine-wide image encoding service shared by the websites of a server")
    set(CPACK_PACKAGE_DESCRIPTION "A daemon (tiraged) with one queue and one CPU budget for the whole machine,
a disposable worker process per encode (libvips), and the tirage command.
Callers send their own encoding profile with each request, through a Unix socket
open to the tirage group.")
    set(CPACK_PACKAGE_HOMEPAGE_URL https://github.com/jcailloux/tirage)
    set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
    set(CPACK_STRIP_FILES ON)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SECTION graphics)
    # libvips and the rest come from dpkg-shlibdeps. The AVIF encoder is only a
    # recommendation of libheif, and without it every AVIF write fails.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libheif-plugin-aomenc, systemd")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        ${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst
        ${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/prerm
        ${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm)
    set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
    include(CPack)
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

    # The caller of docs/integrating.md, built so that it keeps compiling
    # against the headers, and run by the integration harness.
    add_executable(tirage-example examples/encode.cpp)
    target_link_libraries(tirage-example PRIVATE tirage::client)
    target_compile_options(tirage-example PRIVATE ${TIRAGE_WARNINGS})

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
            TIRAGE_EXAMPLE_PATH="$<TARGET_FILE:tirage-example>"
            TIRAGE_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples"
            TIRAGE_VERSION="${PROJECT_VERSION}")
        target_compile_options(tirage-integration PRIVATE ${TIRAGE_WARNINGS})
        add_dependencies(tirage-integration tirage-worker tiraged tirage tirage-example)
        add_test(NAME tirage-integration COMMAND tirage-integration --duration=true)
        set_tests_properties(tirage-integration PROPERTIES LABELS integration)
    endif()
endif()
