cmake_minimum_required(VERSION 3.22.1)
project(EPoToken)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NITRO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(CORE_ROOT  ${NITRO_ROOT}/cpp/core)

# nlohmann/json — fetched once at configure time.
set(JSON_DIR ${CMAKE_BINARY_DIR}/_json/nlohmann)
if(NOT EXISTS ${JSON_DIR}/json.hpp)
    file(MAKE_DIRECTORY ${JSON_DIR})
    file(DOWNLOAD
        https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
        ${JSON_DIR}/json.hpp)
endif()

add_library(EPoToken SHARED
    ${NITRO_ROOT}/cpp/HybridEPoToken.cpp
    ${CORE_ROOT}/src/base64.cpp
    ${CORE_ROOT}/src/challenge.cpp
    ${CORE_ROOT}/src/innertube_messages.cpp
    ${CORE_ROOT}/src/placeholder.cpp
)

target_include_directories(EPoToken PRIVATE
    ${NITRO_ROOT}/cpp
    ${CORE_ROOT}/include
    ${CORE_ROOT}/src
    ${CMAKE_BINARY_DIR}/_json
)

find_package(react-native-nitro-modules REQUIRED CONFIG)
target_link_libraries(EPoToken PRIVATE NitroModules::NitroModules)
