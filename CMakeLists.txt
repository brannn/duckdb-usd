cmake_minimum_required(VERSION 3.5...3.29)

# Set extension name
set(TARGET_NAME usd)

# Project configuration
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

# Require C++17 for std::filesystem and other modern features
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# Find OpenUSD via vcpkg
find_package(pxr CONFIG REQUIRED)

# Extension sources
set(EXTENSION_SOURCES
    src/usd_extension.cpp
    src/usd_prims.cpp
    src/usd_properties.cpp
    src/usd_relationships.cpp
    src/usd_xforms.cpp
    src/usd_helpers.cpp
)

# Build static and loadable extensions using DuckDB's build functions
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Add USD include directories to the extension targets
target_include_directories(${EXTENSION_NAME} PRIVATE ${PXR_INCLUDE_DIRS})
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${PXR_INCLUDE_DIRS})

# Link OpenUSD libraries - use plain signature to match DuckDB's build system
target_link_libraries(${EXTENSION_NAME} usd usdGeom sdf)
target_link_libraries(${LOADABLE_EXTENSION_NAME} usd usdGeom sdf)

# Install static library
install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

