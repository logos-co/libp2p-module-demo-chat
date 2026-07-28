cmake_minimum_required(VERSION 3.14)
project(libp2p-module-demo-chat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED LOGOS_LIBP2P_MODULE_DIR)
    set(LOGOS_LIBP2P_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../logos-libp2p-module")
endif()

get_filename_component(LOGOS_LIBP2P_MODULE_DIR "${LOGOS_LIBP2P_MODULE_DIR}" ABSOLUTE)

if(NOT EXISTS "${LOGOS_LIBP2P_MODULE_DIR}/src/plugin.h")
    message(FATAL_ERROR "logos-libp2p-module not found at ${LOGOS_LIBP2P_MODULE_DIR}. Set -DLOGOS_LIBP2P_MODULE_DIR=/path/to/logos-libp2p-module.")
endif()

find_package(nlohmann_json REQUIRED)

find_path(LIBP2P_INCLUDE_DIR
    NAMES libp2p.h
    PATHS
        ENV LIBP2P_INCLUDE_DIR
        ENV C_INCLUDE_PATH
        ENV CPLUS_INCLUDE_PATH
        "${LOGOS_LIBP2P_MODULE_DIR}/lib"
    PATH_SUFFIXES include
)
if(NOT LIBP2P_INCLUDE_DIR)
    message(FATAL_ERROR "libp2p.h not found. Build inside the logos-libp2p-module Nix development shell.")
endif()

find_library(LIBP2P_LIBRARY
    NAMES p2p
    PATHS
        ENV LIBP2P_LIBRARY_DIR
        ENV LIBRARY_PATH
        ENV LD_LIBRARY_PATH
    PATH_SUFFIXES lib
)
if(NOT LIBP2P_LIBRARY)
    message(FATAL_ERROR "libp2p shared library not found. Build inside the logos-libp2p-module Nix development shell.")
endif()
get_filename_component(LIBP2P_LIBRARY_DIR "${LIBP2P_LIBRARY}" DIRECTORY)

find_path(LOGOS_CPP_SDK_INCLUDE_DIR
    NAMES logos_json.h logos_result.h
    PATHS ENV LOGOS_CPP_SDK_ROOT
    PATH_SUFFIXES include
)
if(NOT LOGOS_CPP_SDK_INCLUDE_DIR)
    message(FATAL_ERROR "Logos C++ SDK headers not found. Build inside the logos-libp2p-module Nix development shell.")
endif()

add_library(logos_libp2p_module_wrapper STATIC
    "${LOGOS_LIBP2P_MODULE_DIR}/src/utils.cpp"
    "${LOGOS_LIBP2P_MODULE_DIR}/src/plugin.cpp"
    "${LOGOS_LIBP2P_MODULE_DIR}/src/callbacks.cpp"
    "${LOGOS_LIBP2P_MODULE_DIR}/src/kademlia.cpp"
    "${LOGOS_LIBP2P_MODULE_DIR}/src/stream.cpp"
    "${LOGOS_LIBP2P_MODULE_DIR}/src/gossipsub.cpp"
    "${LOGOS_LIBP2P_MODULE_DIR}/src/service_discovery.cpp"
    "${LOGOS_LIBP2P_MODULE_DIR}/src/custom_handlers.cpp"
    "${LOGOS_LIBP2P_MODULE_DIR}/src/peerstore.cpp"
    "${LOGOS_LIBP2P_MODULE_DIR}/src/metrics.cpp"
)

target_include_directories(logos_libp2p_module_wrapper PUBLIC
    "${LOGOS_LIBP2P_MODULE_DIR}/src"
    "${LOGOS_LIBP2P_MODULE_DIR}/include"
    "${LOGOS_LIBP2P_MODULE_DIR}/lib"
    "${LIBP2P_INCLUDE_DIR}"
    "${LOGOS_CPP_SDK_INCLUDE_DIR}"
)

target_link_libraries(logos_libp2p_module_wrapper PUBLIC
    "${LIBP2P_LIBRARY}"
    nlohmann_json::nlohmann_json
)

add_executable(demo-chat src/main.cpp)
target_link_libraries(demo-chat PRIVATE logos_libp2p_module_wrapper)

set_target_properties(demo-chat PROPERTIES
    BUILD_RPATH "${CMAKE_BUILD_RPATH};${CMAKE_BINARY_DIR};${CMAKE_CURRENT_BINARY_DIR};${LIBP2P_LIBRARY_DIR}"
    INSTALL_RPATH "$ORIGIN/../lib"
)

install(TARGETS demo-chat RUNTIME DESTINATION bin)
