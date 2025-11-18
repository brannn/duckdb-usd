# DuckDB USD Extension - Example Queries

This document provides a collection of practical SQL queries demonstrating the capabilities of the DuckDB USD extension.

## Table of Contents

- [Basic Queries](#basic-queries)
- [Filtering and Searching](#filtering-and-searching)
- [Spatial Analysis](#spatial-analysis)
- [Relationship Analysis](#relationship-analysis)
- [Property Analysis](#property-analysis)
- [Advanced Patterns](#advanced-patterns)
- [Performance Optimization](#performance-optimization)

## Basic Queries

### List All Prims

```sql
SELECT prim_path, prim_type, active
FROM usd_prims('scene.usda')
ORDER BY prim_path;
```

### Count Prims by Type

```sql
SELECT prim_type, COUNT(*) as count
FROM usd_prims('scene.usda')
GROUP BY prim_type
ORDER BY count DESC;
```

### Get Scene Statistics

```sql
SELECT 
    COUNT(*) as total_prims,
    COUNT(DISTINCT prim_type) as unique_types,
    SUM(CASE WHEN active THEN 1 ELSE 0 END) as active_prims,
    SUM(CASE WHEN instanceable THEN 1 ELSE 0 END) as instanceable_prims
FROM usd_prims('scene.usda');
```

## Filtering and Searching

### Find Prims by Name Pattern

```sql
SELECT prim_path, prim_type, name
FROM usd_prims('datacenter.usd')
WHERE name LIKE '%Server%'
ORDER BY prim_path;
```

### Find Prims at Specific Hierarchy Level

```sql
SELECT prim_path, prim_type, parent_path
FROM usd_prims('scene.usda')
WHERE parent_path = '/World'
ORDER BY name;
```

### Search by Kind

```sql
SELECT prim_path, prim_type, kind
FROM usd_prims('scene.usda')
WHERE kind IN ('component', 'group', 'assembly')
ORDER BY kind, prim_path;
```

### Find Inactive Prims

```sql
SELECT prim_path, prim_type, parent_path
FROM usd_prims('scene.usda')
WHERE active = false;
```

## Spatial Analysis

### Find Objects Near Origin

```sql
SELECT prim_path, x, y, z,
       SQRT(x*x + y*y + z*z) as distance_from_origin
FROM usd_xforms('scene.usda')
WHERE SQRT(x*x + y*y + z*z) < 10.0
ORDER BY distance_from_origin;
```

### Find Objects in Bounding Box

```sql
SELECT prim_path, x, y, z
FROM usd_xforms('datacenter.usd')
WHERE x BETWEEN -100 AND 100
  AND y BETWEEN 0 AND 50
  AND z BETWEEN -100 AND 100
ORDER BY y DESC;
```

### Calculate Distance Between Objects

```sql
WITH positions AS (
    SELECT prim_path, x, y, z
    FROM usd_xforms('scene.usda')
)
SELECT 
    a.prim_path as object_a,
    b.prim_path as object_b,
    SQRT(POWER(b.x - a.x, 2) + POWER(b.y - a.y, 2) + POWER(b.z - a.z, 2)) as distance
FROM positions a
CROSS JOIN positions b
WHERE a.prim_path < b.prim_path
  AND a.prim_path LIKE '%/Equipment/%'
  AND b.prim_path LIKE '%/Equipment/%'
ORDER BY distance
LIMIT 10;
```

### Spatial Density Analysis

```sql
SELECT 
    FLOOR(x / 10) * 10 as grid_x,
    FLOOR(z / 10) * 10 as grid_z,
    COUNT(*) as object_count
FROM usd_xforms('datacenter.usd')
GROUP BY grid_x, grid_z
HAVING object_count > 5
ORDER BY object_count DESC;
```

## Relationship Analysis

### Find All Material Bindings

```sql
SELECT prim_path, target_path
FROM usd_relationships('scene.usda')
WHERE rel_name = 'material:binding'
ORDER BY prim_path;
```

### Analyze Relationship Patterns

```sql
SELECT 
    rel_name,
    COUNT(*) as usage_count,
    COUNT(DISTINCT prim_path) as unique_sources,
    COUNT(DISTINCT target_path) as unique_targets
FROM usd_relationships('scene.usda')
GROUP BY rel_name
ORDER BY usage_count DESC;
```

### Find Multi-Target Relationships

```sql
SELECT prim_path, rel_name, COUNT(*) as target_count
FROM usd_relationships('scene.usda')
GROUP BY prim_path, rel_name
HAVING COUNT(*) > 1
ORDER BY target_count DESC;
```

### Build Dependency Graph

```sql
WITH RECURSIVE dependency_tree AS (
    -- Start with root relationships
    SELECT prim_path, rel_name, target_path, 1 as depth
    FROM usd_relationships('scene.usda')
    WHERE prim_path = '/Root/Equipment/Server_01'
    
    UNION ALL
    
    -- Recursively find dependencies
    SELECT r.prim_path, r.rel_name, r.target_path, dt.depth + 1
    FROM usd_relationships('scene.usda') r
    JOIN dependency_tree dt ON r.prim_path = dt.target_path
    WHERE dt.depth < 5
)
SELECT * FROM dependency_tree
ORDER BY depth, prim_path;
```

## Property Analysis

### Find Properties by Type

```sql
SELECT prop_name, usd_type_name, COUNT(*) as usage_count
FROM usd_properties('scene.usda')
WHERE prop_kind = 'attribute'
GROUP BY prop_name, usd_type_name
ORDER BY usage_count DESC;
```

### Find Time-Sampled Properties

```sql
SELECT prim_path, prop_name, usd_type_name
FROM usd_properties('animated_scene.usda')
WHERE is_time_sampled = true
ORDER BY prim_path, prop_name;
```

### Find Array Properties

```sql
SELECT prim_path, prop_name, usd_type_name, default_value
FROM usd_properties('scene.usda')
WHERE is_array = true
ORDER BY prim_path;
```

### Property Coverage Analysis

```sql
SELECT
    p.prim_type,
    COUNT(DISTINCT pr.prop_name) as unique_properties,
    COUNT(*) as total_property_instances
FROM usd_prims('scene.usda') p
LEFT JOIN usd_properties('scene.usda') pr ON p.prim_path = pr.prim_path
GROUP BY p.prim_type
ORDER BY unique_properties DESC;
```

## Advanced Patterns

### Combine Prims with Transforms

```sql
SELECT
    p.prim_path,
    p.prim_type,
    p.kind,
    x.x, x.y, x.z,
    x.has_rotation,
    x.has_scale
FROM usd_prims('scene.usda') p
JOIN usd_xforms('scene.usda') x ON p.prim_path = x.prim_path
WHERE p.prim_type = 'Mesh'
ORDER BY p.prim_path;
```

### Find Prims with Specific Properties

```sql
SELECT DISTINCT p.prim_path, p.prim_type
FROM usd_prims('scene.usda') p
JOIN usd_properties('scene.usda') pr ON p.prim_path = pr.prim_path
WHERE pr.prop_name = 'visibility'
  AND pr.default_value = 'invisible'
ORDER BY p.prim_path;
```

### Hierarchy Traversal with Window Functions

```sql
SELECT
    prim_path,
    prim_type,
    parent_path,
    LENGTH(prim_path) - LENGTH(REPLACE(prim_path, '/', '')) as depth,
    ROW_NUMBER() OVER (PARTITION BY parent_path ORDER BY name) as sibling_index
FROM usd_prims('scene.usda')
ORDER BY prim_path;
```

### Aggregate Properties by Prim

```sql
SELECT
    p.prim_path,
    p.prim_type,
    COUNT(pr.prop_name) as property_count,
    SUM(CASE WHEN pr.prop_kind = 'attribute' THEN 1 ELSE 0 END) as attribute_count,
    SUM(CASE WHEN pr.prop_kind = 'relationship' THEN 1 ELSE 0 END) as relationship_count
FROM usd_prims('scene.usda') p
LEFT JOIN usd_properties('scene.usda') pr ON p.prim_path = pr.prim_path
GROUP BY p.prim_path, p.prim_type
ORDER BY property_count DESC
LIMIT 20;
```

### Export to CSV

```sql
COPY (
    SELECT p.prim_path, p.prim_type, x.x, x.y, x.z
    FROM usd_prims('datacenter.usd') p
    JOIN usd_xforms('datacenter.usd') x ON p.prim_path = x.prim_path
    WHERE p.prim_type IN ('Xform', 'Mesh')
) TO 'output.csv' (HEADER, DELIMITER ',');
```

### Create Summary Report

```sql
WITH scene_stats AS (
    SELECT
        COUNT(*) as total_prims,
        COUNT(DISTINCT prim_type) as unique_types
    FROM usd_prims('scene.usda')
),
property_stats AS (
    SELECT
        COUNT(*) as total_properties,
        COUNT(DISTINCT prop_name) as unique_properties
    FROM usd_properties('scene.usda')
),
transform_stats AS (
    SELECT
        COUNT(*) as transformable_prims,
        AVG(x) as avg_x, AVG(y) as avg_y, AVG(z) as avg_z
    FROM usd_xforms('scene.usda')
)
SELECT * FROM scene_stats, property_stats, transform_stats;
```

## Performance Optimization

### Use Filters Early

```sql
-- Good: Filter before joining
SELECT p.prim_path, x.x, x.y, x.z
FROM (
    SELECT * FROM usd_prims('large_scene.usd')
    WHERE prim_type = 'Mesh' AND active = true
) p
JOIN usd_xforms('large_scene.usd') x ON p.prim_path = x.prim_path;
```

### Limit Result Sets

```sql
-- Use LIMIT for exploratory queries
SELECT prim_path, prim_type
FROM usd_prims('large_scene.usd')
WHERE prim_type = 'Xform'
LIMIT 100;
```

### Use Specific Columns

```sql
-- Good: Select only needed columns
SELECT prim_path, prim_type
FROM usd_prims('scene.usda')
WHERE active = true;

-- Avoid: SELECT * when not needed
```

### Materialize Intermediate Results

```sql
-- For complex queries, use CTEs or temp tables
CREATE TEMP TABLE active_meshes AS
SELECT prim_path, prim_type, kind
FROM usd_prims('scene.usda')
WHERE prim_type = 'Mesh' AND active = true;

-- Then query the temp table multiple times
SELECT * FROM active_meshes WHERE kind = 'component';
```

## Real-World Use Cases

### Datacenter Infrastructure Inventory

```sql
-- Generate inventory of all equipment with locations
SELECT
    p.prim_path,
    p.name,
    p.prim_type,
    x.x as location_x,
    x.y as floor_level,
    x.z as location_z,
    CASE
        WHEN p.name LIKE '%Server%' THEN 'Server'
        WHEN p.name LIKE '%PDU%' THEN 'Power Distribution'
        WHEN p.name LIKE '%CRAC%' THEN 'Cooling'
        WHEN p.name LIKE '%Switch%' THEN 'Network'
        ELSE 'Other'
    END as equipment_category
FROM usd_prims('datacenter.usd') p
JOIN usd_xforms('datacenter.usd') x ON p.prim_path = x.prim_path
WHERE p.active = true
  AND p.prim_path LIKE '%/Equipment/%'
ORDER BY equipment_category, p.name;
```

### Find Equipment Proximity for Cable Planning

```sql
-- Find pairs of equipment within cable reach (e.g., 5 meters)
WITH equipment_positions AS (
    SELECT
        p.prim_path,
        p.name,
        x.x, x.y, x.z
    FROM usd_prims('datacenter.usd') p
    JOIN usd_xforms('datacenter.usd') x ON p.prim_path = x.prim_path
    WHERE p.prim_path LIKE '%/Equipment/%'
)
SELECT
    a.name as equipment_a,
    b.name as equipment_b,
    ROUND(SQRT(
        POWER(b.x - a.x, 2) +
        POWER(b.y - a.y, 2) +
        POWER(b.z - a.z, 2)
    ), 2) as distance_meters
FROM equipment_positions a
CROSS JOIN equipment_positions b
WHERE a.prim_path < b.prim_path
  AND SQRT(POWER(b.x - a.x, 2) + POWER(b.y - a.y, 2) + POWER(b.z - a.z, 2)) <= 5.0
ORDER BY distance_meters;
```

### Power Dependency Mapping

```sql
-- Map power distribution relationships
SELECT
    r.prim_path as equipment,
    p.prim_type as equipment_type,
    r.target_path as power_source,
    pt.prim_type as source_type
FROM usd_relationships('datacenter.usd') r
JOIN usd_prims('datacenter.usd') p ON r.prim_path = p.prim_path
JOIN usd_prims('datacenter.usd') pt ON r.target_path = pt.prim_path
WHERE r.rel_name LIKE '%power%'
ORDER BY r.target_path, r.prim_path;
```

### Floor Space Utilization

```sql
-- Calculate equipment density per floor
SELECT
    FLOOR(x.y / 3.0) as floor_number,
    COUNT(*) as equipment_count,
    ROUND(AVG(x.x), 2) as avg_x_position,
    ROUND(AVG(x.z), 2) as avg_z_position,
    ROUND(MAX(x.x) - MIN(x.x), 2) as x_span,
    ROUND(MAX(x.z) - MIN(x.z), 2) as z_span
FROM usd_prims('datacenter.usd') p
JOIN usd_xforms('datacenter.usd') x ON p.prim_path = x.prim_path
WHERE p.prim_path LIKE '%/Equipment/%'
GROUP BY floor_number
ORDER BY floor_number;
```

### Asset Validation and Quality Checks

```sql
-- Find potential issues in USD scene
WITH validation_checks AS (
    -- Check for inactive prims
    SELECT
        'Inactive Prim' as issue_type,
        prim_path,
        prim_type,
        'Prim is marked as inactive' as description
    FROM usd_prims('scene.usda')
    WHERE active = false

    UNION ALL

    -- Check for prims without transforms
    SELECT
        'Missing Transform' as issue_type,
        p.prim_path,
        p.prim_type,
        'Xformable prim has no transform data' as description
    FROM usd_prims('scene.usda') p
    LEFT JOIN usd_xforms('scene.usda') x ON p.prim_path = x.prim_path
    WHERE p.prim_type IN ('Xform', 'Mesh', 'Sphere', 'Cube', 'Cylinder')
      AND x.prim_path IS NULL

    UNION ALL

    -- Check for broken relationships
    SELECT
        'Broken Relationship' as issue_type,
        r.prim_path,
        r.rel_name as prim_type,
        'Relationship target does not exist: ' || r.target_path as description
    FROM usd_relationships('scene.usda') r
    LEFT JOIN usd_prims('scene.usda') p ON r.target_path = p.prim_path
    WHERE p.prim_path IS NULL
      AND r.target_path != ''
)
SELECT * FROM validation_checks
ORDER BY issue_type, prim_path;
```

### Material Usage Analysis

```sql
-- Analyze material assignments across the scene
SELECT
    r.target_path as material_path,
    COUNT(DISTINCT r.prim_path) as geometry_count,
    STRING_AGG(DISTINCT p.prim_type, ', ') as geometry_types
FROM usd_relationships('scene.usda') r
JOIN usd_prims('scene.usda') p ON r.prim_path = p.prim_path
WHERE r.rel_name = 'material:binding'
GROUP BY r.target_path
ORDER BY geometry_count DESC;
```

### Scene Complexity Metrics

```sql
-- Calculate scene complexity metrics
WITH hierarchy_depth AS (
    SELECT
        prim_path,
        LENGTH(prim_path) - LENGTH(REPLACE(prim_path, '/', '')) as depth
    FROM usd_prims('scene.usda')
),
prim_stats AS (
    SELECT
        COUNT(*) as total_prims,
        COUNT(DISTINCT prim_type) as unique_types,
        MAX(depth) as max_depth,
        AVG(depth) as avg_depth
    FROM usd_prims('scene.usda') p
    JOIN hierarchy_depth h ON p.prim_path = h.prim_path
),
property_stats AS (
    SELECT
        COUNT(*) as total_properties,
        COUNT(DISTINCT prop_name) as unique_property_names,
        SUM(CASE WHEN is_time_sampled THEN 1 ELSE 0 END) as animated_properties
    FROM usd_properties('scene.usda')
),
relationship_stats AS (
    SELECT
        COUNT(*) as total_relationships,
        COUNT(DISTINCT rel_name) as unique_relationship_types
    FROM usd_relationships('scene.usda')
)
SELECT
    'Scene Complexity Report' as report_name,
    p.total_prims,
    p.unique_types,
    p.max_depth,
    ROUND(p.avg_depth, 2) as avg_depth,
    pr.total_properties,
    pr.unique_property_names,
    pr.animated_properties,
    r.total_relationships,
    r.unique_relationship_types
FROM prim_stats p, property_stats pr, relationship_stats r;
```

### Export Filtered Data for External Tools

```sql
-- Export specific equipment data to Parquet for analysis
COPY (
    SELECT
        p.prim_path,
        p.name,
        p.prim_type,
        x.x, x.y, x.z,
        pr.prop_name,
        pr.default_value
    FROM usd_prims('datacenter.usd') p
    JOIN usd_xforms('datacenter.usd') x ON p.prim_path = x.prim_path
    LEFT JOIN usd_properties('datacenter.usd') pr ON p.prim_path = pr.prim_path
    WHERE p.prim_path LIKE '%/Equipment/%'
      AND p.active = true
) TO 'equipment_export.parquet' (FORMAT PARQUET);
```

### Find Duplicate or Similar Names

```sql
-- Identify potential naming conflicts
SELECT
    name,
    COUNT(*) as occurrence_count,
    STRING_AGG(prim_path, ', ') as paths
FROM usd_prims('scene.usda')
WHERE name != ''
GROUP BY name
HAVING COUNT(*) > 1
ORDER BY occurrence_count DESC;
```

### Geospatial Grid Analysis

```sql
-- Create a heatmap of object density in 10x10 meter grid cells
SELECT
    FLOOR(x / 10) * 10 as grid_x_min,
    FLOOR(x / 10) * 10 + 10 as grid_x_max,
    FLOOR(z / 10) * 10 as grid_z_min,
    FLOOR(z / 10) * 10 + 10 as grid_z_max,
    COUNT(*) as object_count,
    STRING_AGG(DISTINCT p.prim_type, ', ') as object_types
FROM usd_xforms('datacenter.usd') x
JOIN usd_prims('datacenter.usd') p ON x.prim_path = p.prim_path
GROUP BY grid_x_min, grid_x_max, grid_z_min, grid_z_max
HAVING object_count > 0
ORDER BY object_count DESC;
```

## Integration Examples

### Join with External Data

```sql
-- Join USD data with external equipment database
CREATE TEMP TABLE equipment_db AS
SELECT * FROM read_csv('equipment_database.csv');

SELECT
    p.prim_path,
    p.name as usd_name,
    e.serial_number,
    e.purchase_date,
    e.warranty_expiration,
    x.x, x.y, x.z
FROM usd_prims('datacenter.usd') p
JOIN usd_xforms('datacenter.usd') x ON p.prim_path = x.prim_path
LEFT JOIN equipment_db e ON p.name = e.equipment_name
WHERE p.prim_path LIKE '%/Equipment/%'
ORDER BY e.warranty_expiration;
```

### Compare Two USD Files

```sql
-- Find differences between two versions of a scene
WITH scene_v1 AS (
    SELECT prim_path, prim_type, active
    FROM usd_prims('scene_v1.usda')
),
scene_v2 AS (
    SELECT prim_path, prim_type, active
    FROM usd_prims('scene_v2.usda')
)
SELECT
    'Added' as change_type,
    prim_path,
    prim_type
FROM scene_v2
WHERE prim_path NOT IN (SELECT prim_path FROM scene_v1)

UNION ALL

SELECT
    'Removed' as change_type,
    prim_path,
    prim_type
FROM scene_v1
WHERE prim_path NOT IN (SELECT prim_path FROM scene_v2)

UNION ALL

SELECT
    'Modified' as change_type,
    v2.prim_path,
    v2.prim_type
FROM scene_v2 v2
JOIN scene_v1 v1 ON v2.prim_path = v1.prim_path
WHERE v2.active != v1.active
   OR v2.prim_type != v1.prim_type

ORDER BY change_type, prim_path;
```

### Generate Documentation

```sql
-- Create a markdown table of all prims for documentation
SELECT
    '| ' || prim_path || ' | ' ||
    prim_type || ' | ' ||
    COALESCE(kind, 'N/A') || ' | ' ||
    CASE WHEN active THEN 'Yes' ELSE 'No' END || ' |' as markdown_row
FROM usd_prims('scene.usda')
ORDER BY prim_path;
```

## Tips and Best Practices

### 1. Always Filter Early
Apply WHERE clauses before joins to reduce the amount of data processed.

### 2. Use EXPLAIN for Query Optimization
```sql
EXPLAIN SELECT * FROM usd_prims('large_scene.usd') WHERE prim_type = 'Mesh';
```

### 3. Leverage DuckDB's Parallel Processing
DuckDB automatically parallelizes queries. Larger files benefit from this.

### 4. Use Appropriate Data Types
The extension returns appropriate types (VARCHAR, BOOLEAN, DOUBLE) for efficient processing.

### 5. Cache Intermediate Results
For complex multi-step analysis, use temporary tables or CTEs to avoid re-reading the USD file.

### 6. Combine with Other DuckDB Extensions
The USD extension works seamlessly with other DuckDB features like CSV, Parquet, JSON, and httpfs.

---

For more information, see the [User Guide](USER_GUIDE.md) and [README](README.md).
