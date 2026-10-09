# Embed Fury's renderer without configuring its editor/application build.
cmake_minimum_required(VERSION 3.30)
project(ShipFuryRenderer LANGUAGES C CXX)
set(FURY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../fury")
set(FURY_THIRDPARTY_DIR "${FURY_ROOT}/thirdparty")
include("${FURY_ROOT}/cmake/FuryRuntimeTargets.cmake")

# Keep Fury's language requirements local to this dependency subtree.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)

# This isolated build uses Fury's spdlog v2; Shipwright retains spdlog v1.
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "Build spdlog statically")
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "Build spdlog examples")
set(SPDLOG_INSTALL OFF CACHE BOOL "Install spdlog")
add_subdirectory("${FURY_THIRDPARTY_DIR}/spdlog-v2" "${CMAKE_CURRENT_BINARY_DIR}/spdlog")

set(TRACY_ENABLE OFF CACHE BOOL "Enable Fury renderer profiling")
set(TRACY_STATIC ON CACHE BOOL "Build Tracy statically")
add_subdirectory("${FURY_THIRDPARTY_DIR}/tracy-0.14.0" "${CMAKE_CURRENT_BINARY_DIR}/tracy")
add_subdirectory("${FURY_THIRDPARTY_DIR}/glm-1.0.3" "${CMAKE_CURRENT_BINARY_DIR}/glm")
target_compile_definitions(glm-header-only INTERFACE
    GLM_ENABLE_EXPERIMENTAL GLM_FORCE_LEFT_HANDED GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_XYZW_ONLY GLM_FORCE_QUAT_DATA_XYZW
)
add_subdirectory("${FURY_THIRDPARTY_DIR}/boost" "${CMAKE_CURRENT_BINARY_DIR}/boost")

# Only the header libraries consumed by Fury's renderer core are needed here.
add_library(boost_unordered INTERFACE)
target_include_directories(boost_unordered SYSTEM INTERFACE "${FURY_THIRDPARTY_DIR}/boost_unordered-1.88.0/include")
target_link_libraries(boost_unordered INTERFACE Boost::config Boost::container_hash Boost::core Boost::mp11 Boost::throw_exception)
add_library(option INTERFACE)
target_include_directories(option SYSTEM INTERFACE "${FURY_THIRDPARTY_DIR}/option/include")
add_library(stringzilla_header INTERFACE)
target_include_directories(stringzilla_header SYSTEM INTERFACE "${FURY_THIRDPARTY_DIR}/StringZilla/include")
add_subdirectory("${FURY_THIRDPARTY_DIR}/xxHash-0.8.4" "${CMAKE_CURRENT_BINARY_DIR}/xxhash")

add_library(fury_debugging INTERFACE)
add_library(fury::debugging ALIAS fury_debugging)
fury_configure_debugging_target(fury_debugging)
add_library(fury_containers INTERFACE)
add_library(fury::containers ALIAS fury_containers)
fury_configure_containers_target(fury_containers)
add_library(fury_memory STATIC)
add_library(fury::memory ALIAS fury_memory)
fury_configure_memory_target(fury_memory)
add_library(fury_math STATIC)
add_library(fury::math ALIAS fury_math)
fury_configure_math_target(fury_math)
add_library(fury_async STATIC)
add_library(fury::async ALIAS fury_async)
fury_configure_async_target(fury_async)
target_link_libraries(fury_async PUBLIC Threads::Threads)
add_library(fury_reflection INTERFACE)
add_library(fury::reflection ALIAS fury_reflection)
fury_configure_reflection_target(fury_reflection)

set(NRI_STATIC_LIBRARY ON CACHE BOOL "Build NRI statically")
set(NRI_ENABLE_WGPU_SUPPORT OFF CACHE BOOL "Enable NRI WebGPU backend")
set(NRI_ENABLE_NVTX_SUPPORT OFF CACHE BOOL "Enable NRI Nsight annotations")
add_subdirectory("${FURY_THIRDPARTY_DIR}/NRI" "${CMAKE_CURRENT_BINARY_DIR}/NRI")

add_library(fury_device_init STATIC)
add_library(fury::device_init ALIAS fury_device_init)
fury_configure_device_init_target(fury_device_init)
target_link_libraries(fury_device_init PUBLIC fury::memory Vulkan::Vulkan)
if(WIN32)
    target_link_libraries(fury_device_init PUBLIC d3d12 dxgi dxguid)
endif()

set(FURY_BINARY_DEPS_ROOT "$ENV{FURY_BINARY_DEPS_ROOT}" CACHE PATH "Fury binary dependencies, including slang-bin")
include("${FURY_THIRDPARTY_DIR}/cmake/ImportedSlang.cmake")
add_library(fury_filesystem STATIC "${FURY_ROOT}/libs/filesystem/src/Filesystem.cpp")
add_library(fury::filesystem ALIAS fury_filesystem)
target_include_directories(fury_filesystem PUBLIC "${FURY_ROOT}/libs/filesystem/include")
target_link_libraries(fury_filesystem PUBLIC fury::debugging fury::containers PRIVATE fury::memory)

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
add_library(fury_renderer SHARED)
add_library(fury::renderer ALIAS fury_renderer)
fury_configure_renderer_core_target(fury_renderer)
target_compile_definitions(fury_renderer PRIVATE SHIP_FURY_BUILD_RUNTIME)
target_sources(fury_renderer PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../renderer/fury/Runtime.cpp"
    "${FURY_ROOT}/libs/rendering/renderer/src/presentation/SurfaceBackend.cpp"
    "${FURY_ROOT}/libs/rendering/renderer/src/video/VideoDecodeBackendServices.cpp"
    "${FURY_ROOT}/libs/rendering/renderer/src/ShaderCompiler.cpp"
    "${FURY_ROOT}/libs/rendering/renderer/src/ShaderCompilerReflection.cpp"
    "${FURY_ROOT}/libs/rendering/renderer/src/ShaderCompilerSessionCatalog.cpp"
    "${FURY_ROOT}/libs/rendering/renderer/src/ShaderCompilerCompilation.cpp"
)
target_link_libraries(fury_renderer PUBLIC fury::filesystem PRIVATE slang::slang Vulkan::Vulkan)

# Do not export the private static logging/formatting implementations.
if(UNIX AND NOT APPLE)
    target_link_options(fury_renderer PRIVATE "LINKER:--exclude-libs,ALL")
endif()
set_target_properties(fury_renderer PROPERTIES
    VISIBILITY_INLINES_HIDDEN ON
    INSTALL_RPATH "$ORIGIN"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
foreach(_config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${_config}" _config_upper)
    set_target_properties(fury_renderer PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${_config_upper} "${CMAKE_BINARY_DIR}/lib"
        RUNTIME_OUTPUT_DIRECTORY_${_config_upper} "${CMAKE_BINARY_DIR}/bin"
        ARCHIVE_OUTPUT_DIRECTORY_${_config_upper} "${CMAKE_BINARY_DIR}/lib"
    )
endforeach()
include(CTest)
if(BUILD_TESTING)
    add_executable(ship_fury_runtime_tests "${CMAKE_CURRENT_LIST_DIR}/../../renderer/fury/RuntimeTests.cpp")
    target_link_libraries(ship_fury_runtime_tests PRIVATE fury_renderer)
    add_test(NAME ship_fury_runtime COMMAND ship_fury_runtime_tests)
endif()
install(TARGETS fury_renderer LIBRARY DESTINATION . RUNTIME DESTINATION .)
install(IMPORTED_RUNTIME_ARTIFACTS slang::slang LIBRARY DESTINATION . RUNTIME DESTINATION .)
