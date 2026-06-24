cmake_minimum_required(VERSION 3.16)
project(epotoken VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# libcurl
find_package(CURL REQUIRED)

# nlohmann/json (header-only; fetched if not found)
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
        DOWNLOAD_NO_EXTRACT TRUE
        SOURCE_DIR ${CMAKE_BINARY_DIR}/_json
    )
    FetchContent_MakeAvailable(nlohmann_json)
    # Create an interface target pointing at the downloaded header
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE ${CMAKE_BINARY_DIR}/_json)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
endif()

# V8 — searched via pkg-config first, then manual hints.
# Set V8_INCLUDE_DIR and V8_LIBRARIES on the cmake command line to override.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(V8 QUIET v8)
endif()

if(NOT V8_FOUND)
    # Common install paths (node, v8-dev packages, custom builds)
    find_path(V8_INCLUDE_DIR
        NAMES v8.h
        PATHS
            /usr/include/v8
            /usr/local/include/v8
            /usr/include/node
            /usr/local/include/node
            /opt/homebrew/include/v8
            ${V8_ROOT}/include
        DOC "V8 include directory"
    )
    find_library(V8_LIBRARY
        NAMES v8 v8_monolith
        PATHS
            /usr/lib
            /usr/local/lib
            /usr/lib/x86_64-linux-gnu
            /opt/homebrew/lib
            ${V8_ROOT}/lib
        DOC "V8 shared/static library"
    )
    find_library(V8_PLATFORM_LIBRARY
        NAMES v8_libplatform
        PATHS
            /usr/lib
            /usr/local/lib
            /usr/lib/x86_64-linux-gnu
            /opt/homebrew/lib
            ${V8_ROOT}/lib
    )
    if(V8_INCLUDE_DIR AND V8_LIBRARY)
        set(V8_FOUND TRUE)
        set(V8_INCLUDE_DIRS ${V8_INCLUDE_DIR})
        set(V8_LIBRARIES ${V8_LIBRARY})
        if(V8_PLATFORM_LIBRARY)
            list(APPEND V8_LIBRARIES ${V8_PLATFORM_LIBRARY})
        endif()
    else()
        message(FATAL_ERROR
            "V8 not found. Install libv8-dev (Debian/Ubuntu) or node-dev,\n"
            "or pass -DV8_ROOT=<prefix> to cmake.\n"
            "  Found include: ${V8_INCLUDE_DIR}\n"
            "  Found library: ${V8_LIBRARY}"
        )
    endif()
endif()

message(STATUS "V8 include: ${V8_INCLUDE_DIRS}")
message(STATUS "V8 libs:    ${V8_LIBRARIES}")

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------

add_library(epotoken STATIC
    src/base64.cpp
    src/http_client.cpp
    src/challenge.cpp
    src/v8_runner.cpp
    src/epotoken.cpp
)

target_include_directories(epotoken
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${V8_INCLUDE_DIRS}
)

target_link_libraries(epotoken
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        CURL::libcurl
        ${V8_LIBRARIES}
)

# V8 requires these definitions when linking statically.
target_compile_definitions(epotoken PRIVATE
    V8_COMPRESS_POINTERS
    V8_31BIT_SMIS_ON_64BIT_ARCH
)

# ---------------------------------------------------------------------------
# Optional: standalone CLI for testing
# ---------------------------------------------------------------------------

option(BUILD_CLI "Build epotoken_cli test binary" OFF)
if(BUILD_CLI)
    add_executable(epotoken_cli cli/main.cpp)
    target_link_libraries(epotoken_cli PRIVATE epotoken)
    target_include_directories(epotoken_cli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
endif()

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------

install(TARGETS epotoken
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(FILES include/epotoken.h DESTINATION include/epotoken)
