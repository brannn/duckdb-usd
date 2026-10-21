PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=usd
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# `make setup-vcpkg` clones vcpkg at the release used by DuckDB's extension CI
include extension-ci-tools/makefiles/vcpkg.Makefile
