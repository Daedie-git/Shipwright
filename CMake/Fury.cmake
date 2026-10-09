include(ExternalProject)

set(FURY_BINARY_DEPS_ROOT "$ENV{FURY_BINARY_DEPS_ROOT}" CACHE PATH "Fury binary dependencies, including slang-bin")
# Fury's current C++26 sources are built with Clang independently of Shipwright.
find_program(FURY_C_COMPILER NAMES clang REQUIRED)
find_program(FURY_CXX_COMPILER NAMES clang++ REQUIRED)
set(_fury_build "${CMAKE_BINARY_DIR}/fury-renderer-clang")
if(WIN32)
    set(_fury_library "${_fury_build}/bin/fury_renderer.dll")
    set(_fury_byproducts "${_fury_library}" "${_fury_build}/lib/fury_renderer.lib")
else()
    set(_fury_library "${_fury_build}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}fury_renderer${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(_fury_byproducts "${_fury_library}")
endif()

# Fury's spdlog v2 and C++26 requirements stay in a separate build. Shipwright
# continues to use its existing logging library and language standard.
ExternalProject_Add(ship_fury_renderer
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/CMake/FuryRenderer"
    BINARY_DIR "${_fury_build}"
    CMAKE_ARGS
        "-DCMAKE_BUILD_TYPE=$<CONFIG>"
        "-DCMAKE_C_COMPILER=${FURY_C_COMPILER}"
        "-DCMAKE_CXX_COMPILER=${FURY_CXX_COMPILER}"
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DFURY_BINARY_DEPS_ROOT=${FURY_BINARY_DEPS_ROOT}"
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG>
    BUILD_ALWAYS TRUE
    BUILD_BYPRODUCTS ${_fury_byproducts}
    INSTALL_COMMAND ""
)
add_library(fury::renderer SHARED IMPORTED GLOBAL)
set_target_properties(fury::renderer PROPERTIES IMPORTED_LOCATION "${_fury_library}")
if(WIN32)
    set_target_properties(fury::renderer PROPERTIES
        IMPORTED_LOCATION "${_fury_build}/bin/fury_renderer.dll"
        IMPORTED_IMPLIB "${_fury_build}/lib/fury_renderer.lib"
    )
endif()
add_dependencies(fury::renderer ship_fury_renderer)
target_link_libraries(libultraship PRIVATE fury::renderer)

install(CODE "
    execute_process(
        COMMAND \"${CMAKE_COMMAND}\" --install \"${_fury_build}\" --prefix \"\${CMAKE_INSTALL_PREFIX}\" --config \"\${CMAKE_INSTALL_CONFIG_NAME}\"
        COMMAND_ERROR_IS_FATAL ANY
    )
" COMPONENT ship)
