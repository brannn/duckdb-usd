# DuckDB USD Extension

A DuckDB extension that enables SQL-based querying of Universal Scene Description (USD) files. This extension provides table functions for extracting scene graph metadata, property information, relationship graphs, and spatial transforms from USD files.

## Table of Contents

- [Overview](#overview)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Table Functions](#table-functions)
  - [usd_prims](#usd_prims)
  - [usd_properties](#usd_properties)
  - [usd_relationships](#usd_relationships)
  - [usd_xforms](#usd_xforms)
- [Use Cases](#use-cases)
- [Performance](#performance)
- [Limitations](#limitations)
- [Building from Source](#building-from-source)
- [Contributing](#contributing)
- [License](#license)

## Overview

The DuckDB USD Extension bridges the gap between USD's hierarchical scene description format and SQL's relational query capabilities. This enables data engineers and technical designers to analyze USD files using familiar SQL syntax without writing custom USD traversal code.

The extension is designed for analyzing large-scale USD files containing facility layouts, equipment hierarchies, and spatial relationships. Common applications include datacenter infrastructure analysis, facility management, and asset validation workflows.

## Installation

### Prerequisites

- DuckDB v1.4.2 or later
- macOS ARM64 or x86_64 (Linux support planned)

### Loading the Extension

```sql
LOAD './build/release/extension/usd/usd.duckdb_extension';
```

For unsigned extensions during development:

```bash
duckdb -unsigned
```

## Quick Start

```sql
-- Load the extension
LOAD './build/release/extension/usd/usd.duckdb_extension';

-- Query all prims in a USD file
SELECT prim_path, prim_type, kind 
FROM usd_prims('scene.usda') 
LIMIT 10;

-- Find all equipment with specific properties
SELECT p.prim_path, p.prim_type, pr.prop_name, pr.default_value
FROM usd_prims('facility.usd') p
JOIN usd_properties('facility.usd') pr ON p.prim_path = pr.prim_path
WHERE pr.prop_name = 'assetName';

-- Analyze spatial layout
SELECT prim_path, x, y, z
FROM usd_xforms('datacenter.usd')
WHERE x BETWEEN 0 AND 100
  AND z BETWEEN 0 AND 50;
```

## Table Functions

### usd_prims

Extracts prim metadata from the USD scene graph.

**Signature:**
```sql
usd_prims(file_path VARCHAR) -> TABLE (
    prim_path VARCHAR,
    parent_path VARCHAR,
    name VARCHAR,
    prim_type VARCHAR,
    kind VARCHAR,
    active BOOLEAN,
    instanceable BOOLEAN
)
```

**Example:**
```sql
SELECT prim_path, prim_type, kind
FROM usd_prims('facility.usd')
WHERE kind = 'component'
ORDER BY prim_path;
```

### usd_properties

Extracts property information including attributes and relationships.

**Signature:**
```sql
usd_properties(file_path VARCHAR) -> TABLE (
    prim_path VARCHAR,
    prop_name VARCHAR,
    prop_kind VARCHAR,
    usd_type_name VARCHAR,
    is_array BOOLEAN,
    is_time_sampled BOOLEAN,
    default_value VARCHAR
)
```

**Example:**
```sql
SELECT prim_path, prop_name, usd_type_name, default_value
FROM usd_properties('facility.usd')
WHERE prop_kind = 'attribute' AND usd_type_name = 'double';
```

### usd_relationships

Expands relationship targets into individual rows for graph analysis.

**Signature:**
```sql
usd_relationships(file_path VARCHAR) -> TABLE (
    prim_path VARCHAR,
    rel_name VARCHAR,
    target_path VARCHAR,
    target_index INTEGER
)
```

**Example:**
```sql
SELECT prim_path, rel_name, target_path
FROM usd_relationships('facility.usd')
WHERE rel_name = 'powerSource'
ORDER BY prim_path;
```

### usd_xforms

Extracts world-space transform coordinates for spatial analysis.

**Signature:**
```sql
usd_xforms(file_path VARCHAR) -> TABLE (
    prim_path VARCHAR,
    x DOUBLE,
    y DOUBLE,
    z DOUBLE,
    has_rotation BOOLEAN,
    has_scale BOOLEAN
)
```

**Example:**
```sql
SELECT prim_path, x, y, z
FROM usd_xforms('facility.usd')
WHERE SQRT(POW(x, 2) + POW(z, 2)) < 50;
```

## Use Cases

The extension supports various analytical workflows:

**Metadata Validation:** Query prim hierarchies to validate scene structure and ensure required metadata is present across all assets.

**Property Analysis:** Extract and analyze attribute values to identify configuration issues or validate data completeness.

**Graph Analysis:** Traverse relationship networks to understand dependencies and connections between scene elements.

**Spatial Queries:** Perform proximity analysis and spatial filtering based on world-space coordinates.

## Performance

The extension is optimized for large USD files. Performance benchmarks with a 142 MB production file containing 155,000 prims:

| Function | Records | Query Time |
|----------|---------|------------|
| usd_prims | 155,452 | 1.3-1.6s |
| usd_properties | 40,929 | 0.47s |
| usd_relationships | Variable | 0.35s |
| usd_xforms | 8,186 | 0.37s |

Performance characteristics scale linearly with file size and prim count. The extension uses efficient USD APIs including UsdGeomXformCache for transform computation and UsdPrimRange for scene traversal.

## Limitations

**Local Files Only:** The extension currently supports local file paths. Remote file access (S3, HTTP) is deferred pending DuckDB httpfs compatibility improvements.

**Static Time Sampling:** All queries use UsdTimeCode::Default(). Time-varying attribute values and animation data are not currently supported.

**No Variant Support:** Querying across variant sets is not implemented. The extension reads the default variant selection.

## Building from Source

### Requirements

- CMake 3.15 or later
- C++17 compatible compiler
- vcpkg for dependency management
- OpenUSD 25.8 (installed via vcpkg)

### Build Steps

```bash
# Clone the repository
git clone git@github.com:Purple-People-Eaters-Inc/duckdb-usd.git
cd duckdb-usd

# Initialize submodules
git submodule update --init --recursive

# Configure vcpkg toolchain
export VCPKG_TOOLCHAIN_PATH=$(pwd)/vcpkg/scripts/buildsystems/vcpkg.cmake

# Build the extension
cd build/release
make usd_loadable_extension

# Run tests
duckdb -unsigned -c "LOAD './extension/usd/usd.duckdb_extension'; SELECT * FROM usd_test();"
```

### Development

The extension follows standard DuckDB extension development practices. Key source files:

- `src/usd_extension.cpp` - Extension entry point and function registration
- `src/usd_prims.cpp` - Prim enumeration implementation
- `src/usd_properties.cpp` - Property introspection implementation
- `src/usd_relationships.cpp` - Relationship expansion implementation
- `src/usd_xforms.cpp` - Transform extraction implementation
- `src/usd_helpers.cpp` - Shared USD utilities

Tests are located in `test/sql/` and follow DuckDB's SQL test format.

## Contributing

Contributions are welcome. Please ensure all changes include appropriate tests and follow the existing code style.

## License

This project is licensed under the MIT License. See LICENSE file for details.

