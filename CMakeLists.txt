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
        src/innertube_messages.cpp
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

# ---------------------------------------------------------------------------
# Hermes (replaces V8 as the BotGuard JS engine)
# Set HERMES_ROOT to the Hermes install prefix if not in a standard location.
# Download prebuilts from https://github.com/facebook/hermes/releases
# or set HERMES_ROOT to your React Native SDK's hermes directory.
# ---------------------------------------------------------------------------

set(_hermes_hints
    "${HERMES_ROOT}"
    "/usr/local"
    "/opt/homebrew"
    # React Native SDK locations
    "${CMAKE_CURRENT_SOURCE_DIR}/../node_modules/react-native/sdks/hermes"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../node_modules/react-native/sdks/hermes"
    "${CMAKE_CURRENT_SOURCE_DIR}/../node_modules/hermes-engine"
)

find_path(HERMES_INCLUDE_DIR
    NAMES hermes/hermes.h
    PATH_SUFFIXES include
    PATHS ${_hermes_hints}
    DOC "Hermes include directory"
)

find_library(HERMES_LIBRARY
    NAMES hermes hermesvm
    PATH_SUFFIXES lib
    PATHS ${_hermes_hints}
    DOC "Hermes library"
)

if(NOT HERMES_INCLUDE_DIR OR NOT HERMES_LIBRARY)
    message(FATAL_ERROR
        "Hermes not found. Set -DHERMES_ROOT=<prefix> or install from:\n"
        "  https://github.com/facebook/hermes/releases\n"
        "  npm install hermes-engine  (then -DHERMES_ROOT=node_modules/hermes-engine)\n"
        "  Found include: ${HERMES_INCLUDE_DIR}\n"
        "  Found library: ${HERMES_LIBRARY}"
    )
endif()

message(STATUS "Hermes include: ${HERMES_INCLUDE_DIR}")
message(STATUS "Hermes library: ${HERMES_LIBRARY}")

# ---------------------------------------------------------------------------
# Hermes compiler frontend (parser + AST) — used by src/nsig_extract.cpp to
# parse base.js natively (~0.7s / ~85MB for the full 2.5MB file) instead of
# running meriyah inside the interpreted VM.
#
# This needs the Hermes *source* tree (include/hermes/Parser, ESTree.h) and the
# static frontend libraries from a Hermes *build* tree. Point at them with
#   -DHERMES_SRC_ROOT=<hermes-source>  -DHERMES_BUILD_ROOT=<hermes-build>
# ---------------------------------------------------------------------------
set(HERMES_SRC_ROOT   "" CACHE PATH "Hermes source tree (has include/hermes/Parser/JSParser.h)")
set(HERMES_BUILD_ROOT "" CACHE PATH "Hermes build tree (has lib/Parser/libhermesParser.a)")

find_path(HERMES_PARSER_INCLUDE_DIR hermes/Parser/JSParser.h
    PATHS ${HERMES_SRC_ROOT}/include ${HERMES_INCLUDE_DIR})

if(HERMES_PARSER_INCLUDE_DIR AND HERMES_BUILD_ROOT)
    set(_llvh_src_inc ${HERMES_SRC_ROOT}/external/llvh/include)
    set(_llvh_bld_inc ${HERMES_BUILD_ROOT}/external/llvh/include)
    # Locate each static frontend lib in the build tree.
    set(HERMES_COMPILER_LIBS "")
    foreach(_lib hermesParser hermesAST hermesSupport hermesFrontEndDefs
                 hermesSourceMap hermesRegex hermesPlatformUnicode hermesADT
                 LLVHSupport LLVHDemangle)
        find_library(_hc_${_lib} ${_lib}
            PATHS ${HERMES_BUILD_ROOT}
            PATH_SUFFIXES lib lib/Parser lib/AST lib/Support lib/FrontEndDefs
                          lib/SourceMap lib/Regex lib/Platform/Unicode lib/ADT
                          external/llvh/lib/Support external/llvh/lib/Demangle
            NO_DEFAULT_PATH)
        if(_hc_${_lib})
            list(APPEND HERMES_COMPILER_LIBS ${_hc_${_lib}})
        endif()
    endforeach()
    set(HERMES_NATIVE_PARSER ON)
    message(STATUS "Hermes native parser: ON (${HERMES_PARSER_INCLUDE_DIR})")
else()
    set(HERMES_NATIVE_PARSER OFF)
    message(FATAL_ERROR
        "Native nsig parser needs the Hermes source+build tree.\n"
        "  Set -DHERMES_SRC_ROOT=<hermes-source> -DHERMES_BUILD_ROOT=<hermes-build>")
endif()

# ---------------------------------------------------------------------------
# Library target (native: full flow with Hermes + libcurl)
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Embed the rustypipe-style bg_full_bundle.min.js (bgutils-js + jsdom-minimal)
# into a generated header so hermes_runner.cpp can eval it inside the Hermes
# runtime. The bundle exposes globalThis.runBotguard / newMinter / mint.
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

# ---------------------------------------------------------------------------
# Embed the tiny decipher-only bundle (~1 KB) into a generated header. nsig
# extraction is done natively in C++ (src/nsig_extract.cpp) using Hermes's own
# parser, so meriyah is no longer embedded — this bundle only evaluates the
# extracted decipher script and transforms `n`. Regenerate with
# tools/nsig_bundle/build_decipher.mjs. Exposes __epo_setupNsig / __epo_decipherN.
# ---------------------------------------------------------------------------
set(_nsig_src ${CMAKE_CURRENT_SOURCE_DIR}/src/nsig_decipher.min.js)
set(_nsig_hdr ${CMAKE_BINARY_DIR}/_generated/nsig_bundle.h)

add_custom_command(
    OUTPUT  ${_nsig_hdr}
    DEPENDS ${_nsig_src}
    COMMAND ${CMAKE_COMMAND} -E echo "#pragma once" > ${_nsig_hdr}
    COMMAND ${CMAKE_COMMAND} -E echo "// AUTO-GENERATED from src/nsig_decipher.min.js -- do not edit." >> ${_nsig_hdr}
    COMMAND ${CMAKE_COMMAND} -E echo "namespace epotoken {" >> ${_nsig_hdr}
    COMMAND ${CMAKE_COMMAND} -E echo "inline constexpr const char* NSIG_DECIPHER_JS = R\"NSIG_BUNDLE_X9X(" >> ${_nsig_hdr}
    COMMAND cat ${_nsig_src} >> ${_nsig_hdr}
    COMMAND ${CMAKE_COMMAND} -E echo ")NSIG_BUNDLE_X9X\";" >> ${_nsig_hdr}
    COMMAND ${CMAKE_COMMAND} -E echo "} // namespace epotoken" >> ${_nsig_hdr}
    VERBATIM
)
add_custom_target(_gen_nsig_hdr DEPENDS ${_nsig_hdr})

add_library(epotoken STATIC
    src/base64.cpp
    src/http_client.cpp
    src/challenge.cpp
    src/innertube_messages.cpp
    src/innertube_client.cpp
    src/placeholder.cpp
    src/hermes_runner.cpp
    src/nsig_runner.cpp
    src/nsig_extract.cpp
    src/nsig_client.cpp
    src/epotoken.cpp
    ${_bg_full_hdr}
    ${_nsig_hdr}
)
add_dependencies(epotoken _gen_bg_full_hdr _gen_nsig_hdr)

target_include_directories(epotoken
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/_generated
        ${HERMES_INCLUDE_DIR}
        ${HERMES_PARSER_INCLUDE_DIR}
        ${_llvh_src_inc}
        ${_llvh_bld_inc}
)

target_link_libraries(epotoken
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        CURL::libcurl
        ${HERMES_LIBRARY}
        ${HERMES_COMPILER_LIBS}
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
