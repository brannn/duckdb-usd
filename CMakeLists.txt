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

# Extension sources - starting with minimal skeleton
set(EXTENSION_SOURCES src/usd_extension.cpp)

# Build static and loadable extensions using DuckDB's build functions
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Phase 0: No external dependencies yet - just validate build system
# Dependencies will be added in Phase 1

# Install static library
install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

