cmake_minimum_required(VERSION 3.16)
project(epotoken VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# nlohmann/json (header-only; fetched if not found by pkg-config)
# ---------------------------------------------------------------------------

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    # Download json.hpp into <build>/_json/nlohmann/ so the include path
    # <nlohmann/json.hpp> resolves with <build>/_json as the include root.
    set(_json_inc ${CMAKE_BINARY_DIR}/_json/nlohmann/json.hpp)
    if(NOT EXISTS ${_json_inc})
        file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/_json/nlohmann)
        file(DOWNLOAD
            https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
            ${_json_inc}
        )
    endif()
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE ${CMAKE_BINARY_DIR}/_json)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
endif()

# ---------------------------------------------------------------------------
# WASM target (Emscripten toolchain only)
# Exports pure-compute functions; JS host handles HTTP + BotGuard.
# ---------------------------------------------------------------------------

if(EMSCRIPTEN)
    add_executable(epotoken_wasm
        bindings/wasm/epotoken_wasm.cpp
        src/base64.cpp
        src/challenge.cpp
        src/placeholder.cpp
    )

    target_include_directories(epotoken_wasm PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(epotoken_wasm PRIVATE nlohmann_json::nlohmann_json)

    set_target_properties(epotoken_wasm PROPERTIES
        SUFFIX ".js"
    )

    target_link_options(epotoken_wasm PRIVATE
        "SHELL:--bind"
        "SHELL:-s MODULARIZE=1"
        "SHELL:-s EXPORT_NAME='EPoToken'"
        "SHELL:-s ENVIRONMENT=web,node"
        "SHELL:-s SINGLE_FILE=0"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    )

    # Nothing below this point is relevant for WASM builds.
    return()
endif()

# ---------------------------------------------------------------------------
# Native dependencies (not used in WASM builds)
# ---------------------------------------------------------------------------

find_package(CURL REQUIRED)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(V8 QUIET v8)
endif()

if(NOT V8_FOUND)
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
# Library target (native: full flow with V8 + libcurl)
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Embed the rustypipe-style bg_full_bundle.min.js (bgutils-js + jsdom-minimal)
# into a generated header so v8_runner.cpp can eval it inside the embedded V8
# isolate. The bundle exposes globalThis.runBotguard / newMinter / mint.
#
# We splice the raw JS into a C++ raw string literal via cat — cmake's
# file(WRITE) does ${VAR} expansion and \ escape processing on the contents,
# which mangles minified JS that uses template literals and backslash escapes.
# ---------------------------------------------------------------------------
set(_bg_full_src ${CMAKE_CURRENT_SOURCE_DIR}/src/bg_full_bundle.min.js)
set(_bg_full_hdr ${CMAKE_BINARY_DIR}/_generated/bg_full_bundle.h)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/_generated)

add_custom_command(
    OUTPUT  ${_bg_full_hdr}
    DEPENDS ${_bg_full_src}
    COMMAND ${CMAKE_COMMAND} -E echo "#pragma once" > ${_bg_full_hdr}
    COMMAND ${CMAKE_COMMAND} -E echo "// AUTO-GENERATED from src/bg_full_bundle.min.js -- do not edit." >> ${_bg_full_hdr}
    COMMAND ${CMAKE_COMMAND} -E echo "namespace epotoken {" >> ${_bg_full_hdr}
    COMMAND ${CMAKE_COMMAND} -E echo "inline constexpr const char* BG_FULL_BUNDLE_JS = R\"BG_BUNDLE_X9X(" >> ${_bg_full_hdr}
    COMMAND cat ${_bg_full_src} >> ${_bg_full_hdr}
    COMMAND ${CMAKE_COMMAND} -E echo ")BG_BUNDLE_X9X\";" >> ${_bg_full_hdr}
    COMMAND ${CMAKE_COMMAND} -E echo "} // namespace epotoken" >> ${_bg_full_hdr}
    VERBATIM
)
add_custom_target(_gen_bg_full_hdr DEPENDS ${_bg_full_hdr})

add_library(epotoken STATIC
    src/base64.cpp
    src/http_client.cpp
    src/challenge.cpp
    src/innertube_client.cpp
    src/placeholder.cpp
    src/v8_runner.cpp
    src/epotoken.cpp
    ${_bg_full_hdr}
)
add_dependencies(epotoken _gen_bg_full_hdr)

target_include_directories(epotoken
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/_generated
        ${V8_INCLUDE_DIRS}
)

target_link_libraries(epotoken
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        CURL::libcurl
        ${V8_LIBRARIES}
)

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

# ---------------------------------------------------------------------------
# NAPI note: use node-gyp + bindings/napi/binding.gyp
#   cd bindings/napi && npm install && npm run build
# ---------------------------------------------------------------------------
