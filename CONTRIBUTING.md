# Contributing to DuckDB USD Extension

Thank you for your interest in contributing to the DuckDB USD Extension! This document provides guidelines and instructions for developers who want to contribute to the project.

## Table of Contents

- [Development Environment Setup](#development-environment-setup)
- [Building the Extension](#building-the-extension)
- [Running Tests](#running-tests)
- [Code Style and Standards](#code-style-and-standards)
- [Adding New Features](#adding-new-features)
- [Submitting Changes](#submitting-changes)
- [Reporting Issues](#reporting-issues)

## Development Environment Setup

### Prerequisites

- **C++ Compiler**: C++17 compatible compiler (GCC 9+, Clang 10+, MSVC 2019+)
- **CMake**: Version 3.15 or later
- **Git**: For version control
- **Python 3**: For running test scripts
- **vcpkg**: Automatically managed by the build system

### Supported Platforms

- Linux (x86_64, ARM64)
- macOS (x86_64, ARM64)
- Windows (x86_64)
- WebAssembly

### Clone the Repository

```bash
git clone --recurse-submodules git@github.com:Purple-People-Eaters-Inc/duckdb-usd.git
cd duckdb-usd
```

The `--recurse-submodules` flag is important as it clones the DuckDB and extension-ci-tools submodules.

### Install Dependencies

Dependencies are managed through vcpkg and are automatically installed during the build process. The main dependencies are:

- OpenUSD 25.8
- Boost (filesystem)
- TBB (Threading Building Blocks)

## Building the Extension

### Configure and Build

```bash
# Set environment variables
export VCPKG_TOOLCHAIN_PATH=$(pwd)/vcpkg/scripts/buildsystems/vcpkg.cmake

# Create build directory
mkdir -p build/release
cd build/release

# Configure with CMake
cmake -DCMAKE_BUILD_TYPE=Release ../..

# Build the extension
make usd_loadable_extension

# The extension will be at: build/release/extension/usd/usd.duckdb_extension
```

### Build Targets

- `usd_loadable_extension` - Builds the loadable extension (.duckdb_extension)
- `usd_extension` - Builds the static library
- `unittest` - Builds the test runner

### Development Build

For faster iteration during development:

```bash
cmake -DCMAKE_BUILD_TYPE=Debug ../..
make usd_loadable_extension -j$(nproc)
```

## Running Tests

### Test Structure

Tests are located in `test/sql/` and use DuckDB's SQL logic test format:

- `test/sql/usd_test.test` - Basic extension loading
- `test/sql/usd_prims.test` - Prim table function tests
- `test/sql/usd_properties.test` - Property table function tests
- `test/sql/usd_relationships.test` - Relationship table function tests
- `test/sql/usd_xforms.test` - Transform table function tests
- `test/sql/usd_edge_cases.test` - Edge case tests
- `test/sql/usd_error_handling.test` - Error handling tests

### Running All Tests

```bash
cd build/release
make unittest
python3 ../../duckdb/scripts/run_tests_one_by_one.py ./test/unittest "../../test/sql/usd*.test"
```

### Running Specific Tests

```bash
python3 ../../duckdb/scripts/run_tests_one_by_one.py ./test/unittest "../../test/sql/usd_prims.test"
```

### Manual Testing

```bash
# Load the extension in DuckDB
duckdb -unsigned

# In DuckDB shell:
LOAD './build/release/extension/usd/usd.duckdb_extension';
SELECT * FROM usd_test();
SELECT * FROM usd_prims('test/data/simple_scene.usda');
```

## Code Style and Standards

### C++ Style Guidelines

- **Standard**: C++17
- **Naming Conventions**:
  - Classes: `PascalCase` (e.g., `UsdStageManager`)
  - Functions: `PascalCase` for public API, `snake_case` for internal
  - Variables: `snake_case` (e.g., `prim_path`)
  - Constants: `UPPER_SNAKE_CASE`
- **Indentation**: 4 spaces (no tabs)
- **Line Length**: Prefer 100 characters, hard limit at 120

### Code Organization

```
src/
├── include/
│   ├── usd_extension.hpp
│   ├── usd_helpers.hpp
│   ├── usd_prims.hpp
│   ├── usd_properties.hpp
│   ├── usd_relationships.hpp
│   └── usd_xforms.hpp
├── usd_extension.cpp      # Extension entry point
├── usd_helpers.cpp         # USD helper utilities
├── usd_prims.cpp          # usd_prims() table function
├── usd_properties.cpp     # usd_properties() table function
├── usd_relationships.cpp  # usd_relationships() table function
└── usd_xforms.cpp         # usd_xforms() table function
```

### Error Handling

Always provide specific, actionable error messages:

```cpp
// Good
if (!std::filesystem::exists(file_path)) {
    throw BinderException("usd_prims: USD file not found: " + file_path);
}

// Bad
if (!std::filesystem::exists(file_path)) {
    throw BinderException("Invalid file");
}
```

### Input Validation

All table functions must validate:
1. Argument count
2. Argument types
3. File existence
4. File extension (.usd, .usda, .usdc, .usdz)
5. Path is not a directory
6. Path is not empty/whitespace

## Adding New Features

### Adding a New Table Function

1. **Create header file** in `src/include/`:
```cpp
#pragma once
#include "duckdb.hpp"

namespace duckdb {
void RegisterMyNewFunction(DatabaseInstance &db);
}
```

2. **Implement the function** in `src/`:
```cpp
#include "my_new_function.hpp"
#include "usd_helpers.hpp"

namespace duckdb {

struct MyFunctionBindData : public TableFunctionData {
    std::string file_path;
    explicit MyFunctionBindData(std::string path) : file_path(std::move(path)) {}
};

// Implement Bind, Init, and Scan functions
// ...

void RegisterMyNewFunction(DatabaseInstance &db) {
    TableFunction func("my_new_function", {LogicalType::VARCHAR}, 
                       MyFunctionScan, MyFunctionBind, MyFunctionInit);
    ExtensionUtil::RegisterFunction(db, func);
}

} // namespace duckdb
```

3. **Register in extension** (`src/usd_extension.cpp`):
```cpp
#include "my_new_function.hpp"

static void LoadInternal(DatabaseInstance &instance) {
    // ... existing registrations
    RegisterMyNewFunction(instance);
}
```

4. **Add tests** in `test/sql/my_new_function.test`

5. **Update documentation** in README.md and USER_GUIDE.md

### Adding Test Data

Test data files go in `test/data/`:
- Use `.usda` (ASCII) format for readability
- Keep files small and focused
- Include comments explaining the test scenario
- Anonymize any real-world data

Example test file:
```usda
#usda 1.0
(
    doc = "Test file for my_new_function"
)

def Xform "TestPrim"
{
    # Test-specific attributes
}
```

## Submitting Changes

### Before Submitting

1. **Run all tests** and ensure they pass
2. **Build on your platform** without warnings
3. **Update documentation** if adding features
4. **Add tests** for new functionality
5. **Follow code style** guidelines

### Commit Message Format

Use conventional commit format:

```
<type>(<scope>): <subject>

<body>

<footer>
```

Types:
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation changes
- `test`: Adding or updating tests
- `refactor`: Code refactoring
- `perf`: Performance improvements
- `chore`: Build process or auxiliary tool changes

Examples:
```
feat(usd_prims): add support for variant sets

Implement variant set querying in usd_prims table function.
Adds new optional parameter for variant selection.

Closes #123
```

```
test: add edge case tests for empty USD files

- Test empty scene files
- Test minimal scenes with single prim
- Add 22 new edge case tests
```

### Pull Request Process

1. **Fork the repository** on GitHub
2. **Create a feature branch** from `main`:
   ```bash
   git checkout -b feat/my-new-feature
   ```
3. **Make your changes** with clear, focused commits
4. **Push to your fork**:
   ```bash
   git push origin feat/my-new-feature
   ```
5. **Open a Pull Request** on GitHub with:
   - Clear description of changes
   - Reference to related issues
   - Test results
   - Screenshots/examples if applicable

### Code Review

- Be responsive to feedback
- Keep discussions focused and professional
- Update your PR based on review comments
- Squash commits if requested

## Reporting Issues

### Bug Reports

When reporting bugs, include:

1. **DuckDB version**: `SELECT version();`
2. **Extension version**: Check git commit or release tag
3. **Operating system**: OS name and version
4. **USD file details**: File size, format (.usda/.usdc/.usdz)
5. **Minimal reproduction**: Smallest query that reproduces the issue
6. **Expected behavior**: What should happen
7. **Actual behavior**: What actually happens
8. **Error messages**: Full error output

Example:
```
**Environment:**
- DuckDB: v1.4.2
- Extension: commit abc123
- OS: macOS 14.0 ARM64
- USD file: 50MB .usda file with 10K prims

**Query:**
SELECT * FROM usd_prims('scene.usda') WHERE prim_type = 'Mesh';

**Expected:** Returns all mesh prims
**Actual:** Segmentation fault

**Error:**
Segmentation fault (core dumped)
```

### Feature Requests

When requesting features, include:

1. **Use case**: Why is this feature needed?
2. **Proposed solution**: How should it work?
3. **Alternatives considered**: Other approaches you've thought about
4. **Examples**: Sample queries or API usage

## Development Tips

### Debugging

Enable debug builds for better error messages:
```bash
cmake -DCMAKE_BUILD_TYPE=Debug ../..
make usd_loadable_extension
```

Use DuckDB's built-in debugging:
```sql
PRAGMA enable_verification;
PRAGMA enable_profiling;
```

### Performance Profiling

```sql
PRAGMA enable_profiling;
SELECT * FROM usd_prims('large_file.usd');
PRAGMA profiling_output;
```

### Working with USD Files

Useful USD command-line tools:
```bash
# Inspect USD file structure
usdview scene.usda

# Convert between formats
usdcat input.usda -o output.usdc

# Get file info
usdcat --flatten scene.usda
```

### Common Issues

**Issue**: vcpkg dependencies fail to build
**Solution**: Clear vcpkg cache and rebuild:
```bash
rm -rf vcpkg/buildtrees vcpkg/packages
make usd_loadable_extension
```

**Issue**: Extension signature error
**Solution**: Use `-unsigned` flag during development:
```bash
duckdb -unsigned
```

**Issue**: Tests fail with "file not found"
**Solution**: Run tests from the correct directory:
```bash
cd build/release
python3 ../../duckdb/scripts/run_tests_one_by_one.py ./test/unittest "../../test/sql/usd*.test"
```

## Documentation

### Documentation Files

- `README.md` - Overview and quick start
- `USER_GUIDE.md` - Comprehensive usage guide
- `COOKBOOK.md` - Query examples and patterns
- `CONTRIBUTING.md` - This file
- `STATUS.md` - Project status (internal)

### Updating Documentation

When adding features:
1. Update function signatures in README.md
2. Add comprehensive examples to USER_GUIDE.md
3. Add practical queries to COOKBOOK.md
4. Update description.yml if changing extension metadata

### Documentation Style

- Use clear, concise language
- Provide complete, runnable examples
- Include expected output where helpful
- Anonymize any real-world data
- Use consistent terminology

## Community

### Getting Help

- **GitHub Issues**: For bugs and feature requests
- **GitHub Discussions**: For questions and general discussion
- **DuckDB Discord**: #extensions channel

### Code of Conduct

- Be respectful and inclusive
- Focus on constructive feedback
- Help others learn and grow
- Follow the DuckDB community guidelines

## License

By contributing to this project, you agree that your contributions will be licensed under the MIT License.

## Additional Resources

- [DuckDB Extension Development](https://duckdb.org/docs/extensions/overview)
- [OpenUSD Documentation](https://openusd.org)
- [DuckDB SQL Logic Tests](https://github.com/duckdb/duckdb/tree/main/test)
- [vcpkg Documentation](https://vcpkg.io)

---

Thank you for contributing to the DuckDB USD Extension!

