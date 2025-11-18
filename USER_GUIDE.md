# DuckDB USD Extension User Guide

This guide provides comprehensive examples for querying Universal Scene Description (USD) files with SQL using the DuckDB USD Extension.

## Table of Contents

- [Installation](#installation)
- [Basic Usage](#basic-usage)
- [usd_prims: Scene Graph Metadata](#usd_prims-scene-graph-metadata)
- [usd_properties: Property Introspection](#usd_properties-property-introspection)
- [usd_relationships: Graph Analysis](#usd_relationships-graph-analysis)
- [usd_xforms: Spatial Transforms](#usd_xforms-spatial-transforms)
- [Advanced Query Patterns](#advanced-query-patterns)
- [Performance Considerations](#performance-considerations)

## Installation

Install and load the extension in your DuckDB session:

```sql
INSTALL usd FROM community;
LOAD usd;
```

Verify the extension is loaded:

```sql
SELECT * FROM usd_test();
```

Expected output:

```
┌─────────────────────────────────┐
│         usd_test_result         │
│             varchar             │
├─────────────────────────────────┤
│ USD extension loaded correctly! │
└─────────────────────────────────┘
```

## Basic Usage

The extension provides four table functions for querying USD files. Each function takes a file path as its first argument and returns a table with specific columns.

```sql
-- Query scene graph structure
SELECT * FROM usd_prims('facility.usd');

-- Query property values
SELECT * FROM usd_properties('facility.usd');

-- Query relationships
SELECT * FROM usd_relationships('facility.usd');

-- Query spatial transforms
SELECT * FROM usd_xforms('facility.usd');
```

## usd_prims: Scene Graph Metadata

The `usd_prims` function extracts metadata about every prim in the USD scene graph. This is the foundation for understanding scene structure and hierarchy.

### Function Signature

```sql
usd_prims(file_path VARCHAR) -> TABLE (
    prim_path VARCHAR,      -- Full path to the prim
    parent_path VARCHAR,    -- Path to parent prim
    name VARCHAR,           -- Prim name (last component of path)
    prim_type VARCHAR,      -- USD type (Xform, Mesh, Cube, etc.)
    kind VARCHAR,           -- Kind metadata (component, assembly, etc.)
    active BOOLEAN,         -- Whether prim is active
    instanceable BOOLEAN    -- Whether prim is instanceable
)
```

### Example: List All Prims

```sql
SELECT prim_path, prim_type, kind
FROM usd_prims('datacenter.usd')
ORDER BY prim_path
LIMIT 10;
```

Output:

```
┌──────────────────────────┬───────────┬──────────┐
│        prim_path         │ prim_type │   kind   │
│         varchar          │  varchar  │ varchar  │
├──────────────────────────┼───────────┼──────────┤
│ /Facility                │ Xform     │ assembly │
│ /Facility/Floor1         │ Xform     │ group    │
│ /Facility/Floor1/Rack_01 │ Xform     │ component│
│ /Facility/Floor1/Rack_02 │ Xform     │ component│
│ /Facility/Floor1/PDU_A   │ Xform     │ component│
│ /Facility/Floor1/CRAC_01 │ Xform     │ component│
│ /Facility/Floor2         │ Xform     │ group    │
│ /Facility/Floor2/Rack_03 │ Xform     │ component│
│ /Facility/Floor2/Rack_04 │ Xform     │ component│
│ /Facility/Floor2/PDU_B   │ Xform     │ component│
└──────────────────────────┴───────────┴──────────┘
```

### Example: Count Prims by Type

```sql
SELECT prim_type, COUNT(*) as count
FROM usd_prims('datacenter.usd')
GROUP BY prim_type
ORDER BY count DESC;
```

Output:

```
┌───────────┬───────┐
│ prim_type │ count │
│  varchar  │ int64 │
├───────────┼───────┤
│ Xform     │ 8542  │
│ Mesh      │ 1234  │
│ Cube      │ 156   │
│ Sphere    │ 89    │
│ Cylinder  │ 45    │
└───────────┴───────┘
```

### Example: Find Equipment by Kind

```sql
SELECT prim_path, name
FROM usd_prims('datacenter.usd')
WHERE kind = 'component'
  AND prim_path LIKE '%/Rack_%'
ORDER BY prim_path;
```

Output:

```
┌──────────────────────────┬──────────┐
│        prim_path         │   name   │
│         varchar          │ varchar  │
├──────────────────────────┼──────────┤
│ /Facility/Floor1/Rack_01 │ Rack_01  │
│ /Facility/Floor1/Rack_02 │ Rack_02  │
│ /Facility/Floor2/Rack_03 │ Rack_03  │
│ /Facility/Floor2/Rack_04 │ Rack_04  │
└──────────────────────────┴──────────┘
```

### Example: Analyze Hierarchy Depth

```sql
SELECT 
    LENGTH(prim_path) - LENGTH(REPLACE(prim_path, '/', '')) as depth,
    COUNT(*) as prim_count
FROM usd_prims('datacenter.usd')
GROUP BY depth
ORDER BY depth;
```

Output:

```
┌───────┬────────────┐
│ depth │ prim_count │
│ int64 │   int64    │
├───────┼────────────┤
│ 1     │ 1          │
│ 2     │ 12         │
│ 3     │ 456        │
│ 4     │ 2341       │
│ 5     │ 5678       │
└───────┴────────────┘
```

## usd_properties: Property Introspection

The `usd_properties` function extracts all properties (attributes and relationships) from prims. This enables detailed inspection of property values and metadata validation.

### Function Signature

```sql
usd_properties(file_path VARCHAR) -> TABLE (
    prim_path VARCHAR,        -- Path to the prim owning this property
    prop_name VARCHAR,        -- Property name
    prop_kind VARCHAR,        -- 'attribute' or 'relationship'
    usd_type_name VARCHAR,    -- USD type (double, float3, string, etc.)
    is_array BOOLEAN,         -- Whether property is an array
    is_time_sampled BOOLEAN,  -- Whether property has time samples
    default_value VARCHAR     -- String representation of default value
)
```

### Example: Find All Properties on a Prim

```sql
SELECT prop_name, usd_type_name, default_value
FROM usd_properties('datacenter.usd')
WHERE prim_path = '/Facility/Floor1/Rack_01'
ORDER BY prop_name;
```

Output:

```
┌──────────────┬───────────────┬───────────────┐
│  prop_name   │ usd_type_name │ default_value │
│   varchar    │    varchar    │    varchar    │
├──────────────┼───────────────┼───────────────┤
│ assetName    │ string        │ Rack_01       │
│ height       │ double        │ 2.1           │
│ powerRating  │ double        │ 12000.0       │
│ rackUnits    │ int           │ 42            │
│ serialNumber │ string        │ RK-2024-001   │
└──────────────┴───────────────┴───────────────┘
```

### Example: Find All Equipment with Specific Property

```sql
SELECT prim_path, default_value as power_rating
FROM usd_properties('datacenter.usd')
WHERE prop_name = 'powerRating'
  AND usd_type_name = 'double'
ORDER BY CAST(default_value AS DOUBLE) DESC
LIMIT 10;
```

Output:

```
┌──────────────────────────┬──────────────┐
│        prim_path         │ power_rating │
│         varchar          │   varchar    │
├──────────────────────────┼──────────────┤
│ /Facility/Floor1/PDU_A   │ 480000.0     │
│ /Facility/Floor2/PDU_B   │ 480000.0     │
│ /Facility/Floor1/CRAC_01 │ 125000.0     │
│ /Facility/Floor2/CRAC_02 │ 125000.0     │
│ /Facility/Floor1/Rack_01 │ 12000.0      │
│ /Facility/Floor1/Rack_02 │ 12000.0      │
│ /Facility/Floor2/Rack_03 │ 12000.0      │
│ /Facility/Floor2/Rack_04 │ 12000.0      │
└──────────────────────────┴──────────────┘
```

### Example: Validate Required Properties

```sql
WITH required_props AS (
    SELECT UNNEST(['assetName', 'serialNumber', 'powerRating']) as prop_name
),
equipment_prims AS (
    SELECT prim_path
    FROM usd_prims('datacenter.usd')
    WHERE kind = 'component'
)
SELECT
    e.prim_path,
    r.prop_name,
    CASE WHEN p.prop_name IS NULL THEN 'MISSING' ELSE 'OK' END as status
FROM equipment_prims e
CROSS JOIN required_props r
LEFT JOIN usd_properties('datacenter.usd') p
    ON e.prim_path = p.prim_path AND r.prop_name = p.prop_name
WHERE p.prop_name IS NULL
ORDER BY e.prim_path, r.prop_name;
```

Output:

```
┌──────────────────────────┬──────────────┬─────────┐
│        prim_path         │  prop_name   │ status  │
│         varchar          │   varchar    │ varchar │
├──────────────────────────┼──────────────┼─────────┤
│ /Facility/Floor1/CRAC_01 │ serialNumber │ MISSING │
│ /Facility/Floor2/Rack_03 │ powerRating  │ MISSING │
└──────────────────────────┴──────────────┴─────────┘
```

### Example: Identify Array Properties

```sql
SELECT prim_path, prop_name, usd_type_name
FROM usd_properties('datacenter.usd')
WHERE is_array = true
ORDER BY prim_path, prop_name
LIMIT 10;
```

Output:

```
┌──────────────────────────┬──────────────┬───────────────┐
│        prim_path         │  prop_name   │ usd_type_name │
│         varchar          │   varchar    │    varchar    │
├──────────────────────────┼──────────────┼───────────────┤
│ /Facility/Floor1/Rack_01 │ ipAddresses  │ string[]      │
│ /Facility/Floor1/Rack_02 │ ipAddresses  │ string[]      │
│ /Facility/Floor1/PDU_A   │ phases       │ double[]      │
│ /Facility/Floor2/PDU_B   │ phases       │ double[]      │
└──────────────────────────┴──────────────┴───────────────┘
```

## usd_relationships: Graph Analysis

The `usd_relationships` function expands relationship targets into individual rows, enabling graph traversal and dependency analysis.

### Function Signature

```sql
usd_relationships(file_path VARCHAR) -> TABLE (
    prim_path VARCHAR,     -- Path to prim with the relationship
    rel_name VARCHAR,      -- Relationship name
    target_path VARCHAR,   -- Path to target prim
    target_index INTEGER   -- Index in multi-target relationships
)
```

### Example: Power Dependency Analysis

```sql
SELECT prim_path, target_path
FROM usd_relationships('datacenter.usd')
WHERE rel_name = 'powerSource'
ORDER BY prim_path;
```

Output:

```
┌──────────────────────────┬────────────────────────┐
│        prim_path         │      target_path       │
│         varchar          │        varchar         │
├──────────────────────────┼────────────────────────┤
│ /Facility/Floor1/Rack_01 │ /Facility/Floor1/PDU_A │
│ /Facility/Floor1/Rack_02 │ /Facility/Floor1/PDU_A │
│ /Facility/Floor2/Rack_03 │ /Facility/Floor2/PDU_B │
│ /Facility/Floor2/Rack_04 │ /Facility/Floor2/PDU_B │
└──────────────────────────┴────────────────────────┘
```

### Example: Find Equipment with Redundant Power

```sql
SELECT prim_path, COUNT(*) as power_source_count
FROM usd_relationships('datacenter.usd')
WHERE rel_name = 'powerSource'
GROUP BY prim_path
HAVING COUNT(*) > 1
ORDER BY prim_path;
```

Output:

```
┌──────────────────────────┬────────────────────┐
│        prim_path         │ power_source_count │
│         varchar          │       int64        │
├──────────────────────────┼────────────────────┤
│ /Facility/Floor1/Rack_05 │ 2                  │
│ /Facility/Floor2/Rack_08 │ 2                  │
└──────────────────────────┴────────────────────┘
```

### Example: Analyze Power Distribution Load

```sql
SELECT
    r.target_path as pdu,
    COUNT(*) as connected_racks,
    SUM(CAST(p.default_value AS DOUBLE)) as total_load_watts
FROM usd_relationships('datacenter.usd') r
JOIN usd_properties('datacenter.usd') p
    ON r.prim_path = p.prim_path
WHERE r.rel_name = 'powerSource'
  AND p.prop_name = 'powerRating'
GROUP BY r.target_path
ORDER BY total_load_watts DESC;
```

Output:

```
┌────────────────────────┬──────────────────┬───────────────────┐
│          pdu           │ connected_racks  │ total_load_watts  │
│        varchar         │      int64       │      double       │
├────────────────────────┼──────────────────┼───────────────────┤
│ /Facility/Floor1/PDU_A │ 24               │ 288000.0          │
│ /Facility/Floor2/PDU_B │ 22               │ 264000.0          │
│ /Facility/Floor3/PDU_C │ 18               │ 216000.0          │
└────────────────────────┴──────────────────┴───────────────────┘
```

### Example: Network Topology Analysis

```sql
SELECT
    r.prim_path as server,
    r.target_path as switch,
    pr.name as switch_name
FROM usd_relationships('datacenter.usd') r
JOIN usd_prims('datacenter.usd') pr
    ON r.target_path = pr.prim_path
WHERE r.rel_name = 'networkUplink'
  AND pr.prim_type = 'NetworkSwitch'
ORDER BY switch, server;
```

Output:

```
┌──────────────────────────────┬──────────────────────────┬─────────────┐
│           server             │         switch           │ switch_name │
│           varchar            │         varchar          │   varchar   │
├──────────────────────────────┼──────────────────────────┼─────────────┤
│ /Facility/Floor1/Rack_01/Srv │ /Facility/Floor1/Switch1 │ Switch1     │
│ /Facility/Floor1/Rack_02/Srv │ /Facility/Floor1/Switch1 │ Switch1     │
│ /Facility/Floor1/Rack_03/Srv │ /Facility/Floor1/Switch2 │ Switch2     │
└──────────────────────────────┴──────────────────────────┴─────────────┘
```

## usd_xforms: Spatial Transforms

The `usd_xforms` function extracts world-space transform coordinates for spatial analysis and proximity queries.

### Function Signature

```sql
usd_xforms(file_path VARCHAR) -> TABLE (
    prim_path VARCHAR,      -- Path to transformable prim
    x DOUBLE,               -- X coordinate in world space
    y DOUBLE,               -- Y coordinate in world space
    z DOUBLE,               -- Z coordinate in world space
    has_rotation BOOLEAN,   -- Whether transform includes rotation
    has_scale BOOLEAN       -- Whether transform includes scale
)
```

### Example: Extract Equipment Positions

```sql
SELECT prim_path, x, y, z
FROM usd_xforms('datacenter.usd')
WHERE prim_path LIKE '%/Rack_%'
ORDER BY x, z
LIMIT 10;
```

Output:

```
┌──────────────────────────┬───────┬──────┬───────┐
│        prim_path         │   x   │  y   │   z   │
│         varchar          │ double│double│ double│
├──────────────────────────┼───────┼──────┼───────┤
│ /Facility/Floor1/Rack_01 │ 10.0  │ 0.0  │ 5.0   │
│ /Facility/Floor1/Rack_02 │ 10.0  │ 0.0  │ 10.0  │
│ /Facility/Floor1/Rack_03 │ 10.0  │ 0.0  │ 15.0  │
│ /Facility/Floor1/Rack_04 │ 20.0  │ 0.0  │ 5.0   │
│ /Facility/Floor1/Rack_05 │ 20.0  │ 0.0  │ 10.0  │
│ /Facility/Floor1/Rack_06 │ 20.0  │ 0.0  │ 15.0  │
│ /Facility/Floor2/Rack_07 │ 30.0  │ 3.5  │ 5.0   │
│ /Facility/Floor2/Rack_08 │ 30.0  │ 3.5  │ 10.0  │
│ /Facility/Floor2/Rack_09 │ 30.0  │ 3.5  │ 15.0  │
│ /Facility/Floor2/Rack_10 │ 40.0  │ 3.5  │ 5.0   │
└──────────────────────────┴───────┴──────┴───────┘
```

### Example: Proximity Analysis - Find Nearest Cooling Unit

```sql
WITH rack_positions AS (
    SELECT prim_path, x, y, z
    FROM usd_xforms('datacenter.usd')
    WHERE prim_path LIKE '%/Rack_%'
),
crac_positions AS (
    SELECT prim_path, x, y, z
    FROM usd_xforms('datacenter.usd')
    WHERE prim_path LIKE '%/CRAC_%'
)
SELECT
    r.prim_path as rack,
    c.prim_path as nearest_crac,
    ROUND(SQRT(POW(r.x - c.x, 2) + POW(r.y - c.y, 2) + POW(r.z - c.z, 2)), 2) as distance_meters
FROM rack_positions r
CROSS JOIN crac_positions c
QUALIFY ROW_NUMBER() OVER (PARTITION BY r.prim_path ORDER BY distance_meters) = 1
ORDER BY distance_meters DESC
LIMIT 10;
```

Output:

```
┌──────────────────────────┬──────────────────────────┬─────────────────┐
│          rack            │      nearest_crac        │ distance_meters │
│         varchar          │         varchar          │     double      │
├──────────────────────────┼──────────────────────────┼─────────────────┤
│ /Facility/Floor3/Rack_45 │ /Facility/Floor3/CRAC_05 │ 28.45           │
│ /Facility/Floor2/Rack_32 │ /Facility/Floor2/CRAC_03 │ 24.12           │
│ /Facility/Floor1/Rack_18 │ /Facility/Floor1/CRAC_01 │ 22.87           │
│ /Facility/Floor3/Rack_41 │ /Facility/Floor3/CRAC_05 │ 21.34           │
│ /Facility/Floor2/Rack_28 │ /Facility/Floor2/CRAC_04 │ 19.56           │
└──────────────────────────┴──────────────────────────┴─────────────────┘
```

### Example: Spatial Filtering - Equipment in Specific Zone

```sql
SELECT prim_path, x, z
FROM usd_xforms('datacenter.usd')
WHERE x BETWEEN 10.0 AND 30.0
  AND z BETWEEN 0.0 AND 20.0
  AND prim_path LIKE '%/Rack_%'
ORDER BY x, z;
```

Output:

```
┌──────────────────────────┬───────┬───────┐
│        prim_path         │   x   │   z   │
│         varchar          │ double│ double│
├──────────────────────────┼───────┼───────┤
│ /Facility/Floor1/Rack_01 │ 10.0  │ 5.0   │
│ /Facility/Floor1/Rack_02 │ 10.0  │ 10.0  │
│ /Facility/Floor1/Rack_03 │ 10.0  │ 15.0  │
│ /Facility/Floor1/Rack_04 │ 20.0  │ 5.0   │
│ /Facility/Floor1/Rack_05 │ 20.0  │ 10.0  │
│ /Facility/Floor1/Rack_06 │ 20.0  │ 15.0  │
│ /Facility/Floor2/Rack_07 │ 30.0  │ 5.0   │
│ /Facility/Floor2/Rack_08 │ 30.0  │ 10.0  │
└──────────────────────────┴───────┴───────┘
```

### Example: Calculate Bounding Box

```sql
SELECT
    MIN(x) as min_x,
    MAX(x) as max_x,
    MIN(z) as min_z,
    MAX(z) as max_z,
    ROUND(MAX(x) - MIN(x), 2) as width,
    ROUND(MAX(z) - MIN(z), 2) as depth
FROM usd_xforms('datacenter.usd')
WHERE prim_path LIKE '/Facility/Floor1/%';
```

Output:

```
┌───────┬───────┬───────┬───────┬────────┬────────┐
│ min_x │ max_x │ min_z │ max_z │ width  │ depth  │
│ double│ double│ double│ double│ double │ double │
├───────┼───────┼───────┼───────┼────────┼────────┤
│ 5.0   │ 95.0  │ 2.0   │ 48.0  │ 90.0   │ 46.0   │
└───────┴───────┴───────┴───────┴────────┴────────┘
```

### Example: Identify Scaled or Rotated Equipment

```sql
SELECT prim_path, has_scale, has_rotation
FROM usd_xforms('datacenter.usd')
WHERE has_scale = true OR has_rotation = true
ORDER BY prim_path;
```

Output:

```
┌──────────────────────────────┬───────────┬──────────────┐
│          prim_path           │ has_scale │ has_rotation │
│           varchar            │  boolean  │   boolean    │
├──────────────────────────────┼───────────┼──────────────┤
│ /Facility/Floor1/CustomRack  │ true      │ false        │
│ /Facility/Floor2/AngledUnit  │ false     │ true         │
└──────────────────────────────┴───────────┴──────────────┘
```

## Advanced Query Patterns

### Combining Multiple Table Functions

Join multiple table functions to perform complex analysis combining metadata, properties, relationships, and spatial data.

#### Example: Equipment Inventory with Power and Location

```sql
SELECT
    pr.prim_path,
    pr.name,
    pr.kind,
    CAST(p_power.default_value AS DOUBLE) as power_rating,
    CAST(p_serial.default_value AS VARCHAR) as serial_number,
    x.x,
    x.z,
    r.target_path as power_source
FROM usd_prims('datacenter.usd') pr
LEFT JOIN usd_properties('datacenter.usd') p_power
    ON pr.prim_path = p_power.prim_path AND p_power.prop_name = 'powerRating'
LEFT JOIN usd_properties('datacenter.usd') p_serial
    ON pr.prim_path = p_serial.prim_path AND p_serial.prop_name = 'serialNumber'
LEFT JOIN usd_xforms('datacenter.usd') x
    ON pr.prim_path = x.prim_path
LEFT JOIN usd_relationships('datacenter.usd') r
    ON pr.prim_path = r.prim_path AND r.rel_name = 'powerSource'
WHERE pr.kind = 'component'
  AND pr.prim_path LIKE '%/Rack_%'
ORDER BY pr.prim_path
LIMIT 5;
```

Output:

```
┌──────────────────────────┬─────────┬───────────┬──────────────┬───────────────┬──────┬──────┬────────────────────────┐
│        prim_path         │  name   │   kind    │ power_rating │ serial_number │  x   │  z   │     power_source       │
│         varchar          │ varchar │  varchar  │    double    │    varchar    │double│double│        varchar         │
├──────────────────────────┼─────────┼───────────┼──────────────┼───────────────┼──────┼──────┼────────────────────────┤
│ /Facility/Floor1/Rack_01 │ Rack_01 │ component │ 12000.0      │ RK-2024-001   │ 10.0 │ 5.0  │ /Facility/Floor1/PDU_A │
│ /Facility/Floor1/Rack_02 │ Rack_02 │ component │ 12000.0      │ RK-2024-002   │ 10.0 │ 10.0 │ /Facility/Floor1/PDU_A │
│ /Facility/Floor1/Rack_03 │ Rack_03 │ component │ 12000.0      │ RK-2024-003   │ 10.0 │ 15.0 │ /Facility/Floor1/PDU_B │
│ /Facility/Floor1/Rack_04 │ Rack_04 │ component │ 12000.0      │ RK-2024-004   │ 20.0 │ 5.0  │ /Facility/Floor1/PDU_B │
│ /Facility/Floor1/Rack_05 │ Rack_05 │ component │ 15000.0      │ RK-2024-005   │ 20.0 │ 10.0 │ /Facility/Floor1/PDU_A │
└──────────────────────────┴─────────┴───────────┴──────────────┴───────────────┴──────┴──────┴────────────────────────┘
```

### Recursive Hierarchy Traversal

Use recursive CTEs to traverse the USD hierarchy and analyze parent-child relationships.

#### Example: Find All Descendants of a Prim

```sql
WITH RECURSIVE descendants AS (
    -- Base case: start with the root prim
    SELECT prim_path, parent_path, name, 0 as depth
    FROM usd_prims('datacenter.usd')
    WHERE prim_path = '/Facility/Floor1'

    UNION ALL

    -- Recursive case: find children
    SELECT p.prim_path, p.parent_path, p.name, d.depth + 1
    FROM usd_prims('datacenter.usd') p
    JOIN descendants d ON p.parent_path = d.prim_path
)
SELECT prim_path, depth
FROM descendants
ORDER BY depth, prim_path;
```

Output:

```
┌──────────────────────────────┬───────┐
│          prim_path           │ depth │
│           varchar            │ int64 │
├──────────────────────────────┼───────┤
│ /Facility/Floor1             │ 0     │
│ /Facility/Floor1/CRAC_01     │ 1     │
│ /Facility/Floor1/PDU_A       │ 1     │
│ /Facility/Floor1/Rack_01     │ 1     │
│ /Facility/Floor1/Rack_02     │ 1     │
│ /Facility/Floor1/Rack_01/Srv │ 2     │
│ /Facility/Floor1/Rack_02/Srv │ 2     │
└──────────────────────────────┴───────┘
```

### Aggregation and Statistics

Perform statistical analysis on USD data to understand scene composition and validate data quality.

#### Example: Property Value Distribution

```sql
SELECT
    prop_name,
    usd_type_name,
    COUNT(*) as occurrence_count,
    COUNT(DISTINCT default_value) as unique_values,
    MIN(LENGTH(default_value)) as min_length,
    MAX(LENGTH(default_value)) as max_length
FROM usd_properties('datacenter.usd')
WHERE prop_kind = 'attribute'
GROUP BY prop_name, usd_type_name
HAVING COUNT(*) > 10
ORDER BY occurrence_count DESC
LIMIT 10;
```

Output:

```
┌──────────────┬───────────────┬──────────────────┬───────────────┬────────────┬────────────┐
│  prop_name   │ usd_type_name │ occurrence_count │ unique_values │ min_length │ max_length │
│   varchar    │    varchar    │      int64       │     int64     │   int64    │   int64    │
├──────────────┼───────────────┼──────────────────┼───────────────┼────────────┼────────────┤
│ assetName    │ string        │ 8542             │ 8542          │ 6          │ 32         │
│ powerRating  │ double        │ 2341             │ 12            │ 6          │ 10         │
│ serialNumber │ string        │ 2341             │ 2341          │ 11         │ 15         │
│ height       │ double        │ 1234             │ 8             │ 3          │ 5          │
│ rackUnits    │ int           │ 856              │ 3             │ 2          │ 2          │
└──────────────┴───────────────┴──────────────────┴───────────────┴────────────┴────────────┘
```

### Geospatial Analysis

Perform advanced spatial queries using the transform data.

#### Example: Density Heatmap - Equipment Count per Grid Cell

```sql
WITH grid_cells AS (
    SELECT
        FLOOR(x / 10.0) * 10 as grid_x,
        FLOOR(z / 10.0) * 10 as grid_z,
        COUNT(*) as equipment_count
    FROM usd_xforms('datacenter.usd')
    WHERE prim_path LIKE '%/Rack_%'
    GROUP BY grid_x, grid_z
)
SELECT
    grid_x,
    grid_z,
    equipment_count,
    CASE
        WHEN equipment_count > 20 THEN 'HIGH'
        WHEN equipment_count > 10 THEN 'MEDIUM'
        ELSE 'LOW'
    END as density
FROM grid_cells
ORDER BY equipment_count DESC;
```

Output:

```
┌────────┬────────┬──────────────────┬─────────┐
│ grid_x │ grid_z │ equipment_count  │ density │
│ double │ double │      int64       │ varchar │
├────────┼────────┼──────────────────┼─────────┤
│ 20.0   │ 10.0   │ 28               │ HIGH    │
│ 10.0   │ 10.0   │ 24               │ HIGH    │
│ 30.0   │ 20.0   │ 22               │ HIGH    │
│ 40.0   │ 10.0   │ 18               │ MEDIUM  │
│ 50.0   │ 0.0    │ 12               │ MEDIUM  │
│ 60.0   │ 10.0   │ 8                │ LOW     │
└────────┴────────┴──────────────────┴─────────┘
```

## Performance Considerations

### Query Optimization

The extension reads USD files from disk for each table function call. For queries that reference the same file multiple times, consider using CTEs or temporary tables to cache results.

#### Inefficient: Multiple File Reads

```sql
-- This reads the file 3 times
SELECT p.prim_path, pr.default_value, x.x, x.z
FROM usd_prims('large_file.usd') p
JOIN usd_properties('large_file.usd') pr ON p.prim_path = pr.prim_path
JOIN usd_xforms('large_file.usd') x ON p.prim_path = x.prim_path;
```

#### Efficient: Single File Read with CTEs

```sql
-- This reads the file once per table function
WITH prims AS (
    SELECT * FROM usd_prims('large_file.usd')
),
props AS (
    SELECT * FROM usd_properties('large_file.usd')
),
xforms AS (
    SELECT * FROM usd_xforms('large_file.usd')
)
SELECT p.prim_path, pr.default_value, x.x, x.z
FROM prims p
JOIN props pr ON p.prim_path = pr.prim_path
JOIN xforms x ON p.prim_path = x.prim_path;
```

### Filtering Early

Apply filters as early as possible to reduce the amount of data processed.

```sql
-- Filter in the table function call when possible
SELECT prim_path, x, z
FROM usd_xforms('datacenter.usd')
WHERE prim_path LIKE '%/Rack_%'  -- Filter applied during scan
  AND x > 10.0;                   -- Additional filter
```

### Large File Performance

Performance scales linearly with file size and prim count. Benchmark results with a 142 MB file containing 155,000 prims:

```
Function            Records    Query Time
-----------------------------------------
usd_prims           155,452    1.3-1.6s
usd_properties      40,929     0.47s
usd_relationships   Variable   0.35s
usd_xforms          8,186      0.37s
```

For very large files, consider:

- Filtering by path patterns to reduce result set size
- Using LIMIT clauses during exploratory analysis
- Creating materialized views for frequently accessed data
- Splitting large USD files into smaller components

### Memory Usage

The extension uses DuckDB's streaming execution model and processes data in batches of 2048 rows. Memory usage is proportional to the complexity of the USD stage and the number of concurrent queries.


