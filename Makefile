PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=usd
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# The test runner skips, rather than fails, a test whose extension does not load. CI sets USD_FAIL_SKIPPED_TESTS, so
# that unittest's --require fails those tests instead.
ifneq ($(USD_FAIL_SKIPPED_TESTS),)
TEST_RUNNER += --test-command '{binary} {flags} --require usd --use-colour yes -f {test_list}'
endif

# `make setup-vcpkg` clones vcpkg at the release used by DuckDB's extension CI
include extension-ci-tools/makefiles/vcpkg.Makefile
