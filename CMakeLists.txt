# 3.24: OpenUSD is linked as whole archives ($<LINK_LIBRARY:WHOLE_ARCHIVE>)
cmake_minimum_required(VERSION 3.24...3.29)

set(TARGET_NAME usd)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OpenUSD comes from vcpkg: the overlay port in vcpkg_ports/usd builds it as
# static libraries
find_package(pxr CONFIG REQUIRED)

include_directories(src/include)

set(EXTENSION_SOURCES
    src/usd_extension.cpp
    src/usd_file_access.cpp
    src/usd_json.cpp
    src/usd_plugins.cpp
    src/usd_scan.cpp
    src/usd_stage.cpp
    src/functions/usd_bounds.cpp
    src/functions/usd_composition_errors.cpp
    src/functions/usd_composition_arcs.cpp
    src/functions/usd_material_bindings.cpp
    src/functions/usd_layers.cpp
    src/functions/usd_prims.cpp
    src/functions/usd_properties.cpp
    src/functions/usd_relationships.cpp
    src/functions/usd_stage_metadata.cpp
    src/functions/usd_time_samples.cpp
    src/functions/usd_variants.cpp
    src/functions/usd_version.cpp
    src/functions/usd_xforms.cpp)

# OpenUSD's schema libraries (all but the renderer-specific usdRi and usdHydra),
# in dependency order
set(USD_SCHEMA_LIBRARIES
    usd
    usdGeom
    usdVol
    usdMedia
    usdShade
    usdLux
    usdProc
    usdRender
    usdSemantics
    usdSkel
    usdUI
    usdPhysics)

# The OpenUSD libraries the extension uses, in dependency order
set(USD_LIBRARIES
    arch
    tf
    gf
    pegtl
    js
    trace
    work
    plug
    vt
    ts
    ar
    kind
    sdf
    sdr
    pcp
    ${USD_SCHEMA_LIBRARIES})

get_target_property(USD_LIBRARY_TYPE usd TYPE)
if(NOT USD_LIBRARY_TYPE STREQUAL "STATIC_LIBRARY")
  message(
    FATAL_ERROR
      "The extension links OpenUSD statically: build it with the vcpkg port in vcpkg_ports/usd"
  )
endif()
# The plugin resources (plugInfo.json, generatedSchema.usda) are compiled into
# the extension
include(${CMAKE_CURRENT_LIST_DIR}/cmake/UsdEmbedResources.cmake)
get_filename_component(USD_INSTALL_PREFIX "${PXR_CMAKE_DIR}/../.." ABSOLUTE)
usd_embed_resources("${CMAKE_CURRENT_BINARY_DIR}"
                    "${USD_INSTALL_PREFIX}/lib/usd" ${USD_SCHEMA_LIBRARIES})
list(APPEND EXTENSION_SOURCES ${USD_EMBEDDED_RESOURCES_SOURCE})
# Whole archives: OpenUSD registers types, schemas and file formats from static
# initializers, which the linker would otherwise drop. CMake turns this into
# -force_load or --whole-archive.
list(JOIN USD_LIBRARIES "," USD_LIBRARY_LIST)
set(USD_LINK_LIBRARIES "$<LINK_LIBRARY:WHOLE_ARCHIVE,${USD_LIBRARY_LIST}>")

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

foreach(TARGET ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
  target_include_directories(${TARGET} PRIVATE ${PXR_INCLUDE_DIRS})
  # plain signature, as used by DuckDB's extension build functions
  target_link_libraries(${TARGET} ${USD_LINK_LIBRARIES})
  # DuckDB compiles extensions with hidden visibility, but OpenUSD is built with
  # default visibility. Mixing the two duplicates the template statics OpenUSD
  # uses to identify the types held by a VtValue, which breaks it (failed
  # IsHolding<T> checks, crashes in the asset resolver) - so match OpenUSD's
  # visibility.
  set_target_properties(${TARGET} PROPERTIES CXX_VISIBILITY_PRESET default
                                             VISIBILITY_INLINES_HIDDEN OFF)
endforeach()

# The loadable extension must still only export its entry point. macOS: DuckDB
# passes an exported symbols list. Linux: a version script.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(USD_EXPORTS_MAP "${CMAKE_CURRENT_BINARY_DIR}/usd_exports.map")
  file(WRITE "${USD_EXPORTS_MAP}"
       "{ global: ${TARGET_NAME}_duckdb_cpp_init; local: *; };\n")
  target_link_options(${LOADABLE_EXTENSION_NAME} PRIVATE
                      "-Wl,--version-script=${USD_EXPORTS_MAP}")
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
