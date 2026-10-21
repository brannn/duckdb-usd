# Cookbook

Recipes for common questions about a USD scene. Most examples use a facility laid out as `/Site/<zone>/<unit>`;
the sections on time, physics, labels and characters use a vehicle yard, a robot cell and a crowd. Substitute your
own paths and property names.

- [Inventory](#inventory)
- [Metadata and validation](#metadata-and-validation)
- [Spatial queries](#spatial-queries)
- [Maps and GIS](#maps-and-gis)
- [Graphs of relationships](#graphs-of-relationships)
- [Materials](#materials)
- [Moving things and recorded data](#moving-things-and-recorded-data)
- [Robots and physics](#robots-and-physics)
- [Semantic labels](#semantic-labels)
- [Characters](#characters)
- [Composition health](#composition-health)
- [Many files](#many-files)
- [Large scenes](#large-scenes)

## Inventory

How big is the scene, and what is in it?

```sql
SELECT count(*) AS prims, count(DISTINCT prim_type) AS types, max(depth) AS max_depth
FROM usd_prims('facility.usd');

SELECT prim_type, kind, count(*)
FROM usd_prims('facility.usd')
GROUP BY ALL
ORDER BY count(*) DESC;
```

Count what sits under each zone (the second path element):

```sql
SELECT split_part(prim_path, '/', 3) AS zone, count(*) AS prims
FROM usd_prims('facility.usd')
WHERE depth >= 2
GROUP BY zone
ORDER BY prims DESC;
```

Instances are visited, their contents are not. To count everything — including the parts inside each instance —
visit instance proxies:

```sql
SELECT instance_proxy, count(*)
FROM usd_prims('facility.usd', instance_proxies := true)
GROUP BY ALL;
```

Deactivated (non-destructively deleted) prims:

```sql
SELECT prim_path FROM usd_prims('facility.usd') WHERE NOT active;
```

## Metadata and validation

Every component should carry an `assetInfo` identifier:

```sql
SELECT prim_path
FROM usd_prims('facility.usd')
WHERE kind = 'component' AND asset_info.identifier IS NULL;
```

Custom (non-schema) attributes and their values:

```sql
SELECT prim_path, property_name, type_name, value
FROM usd_properties('facility.usd', authored_only := true)
WHERE is_custom
ORDER BY ALL;
```

Required attributes that are missing:

```sql
WITH required(property_name) AS (VALUES ('serial'), ('rating')),
units AS (SELECT prim_path FROM usd_prims('facility.usd') WHERE kind = 'component')
SELECT u.prim_path, r.property_name AS missing
FROM units u CROSS JOIN required r
ANTI JOIN usd_properties('facility.usd', authored_only := true) p USING (prim_path, property_name)
ORDER BY ALL;
```

Numeric attributes are typed values: aggregate them directly.

```sql
SELECT split_part(prim_path, '/', 3) AS zone, sum(value::DOUBLE) AS total_rating
FROM usd_properties('facility.usd')
WHERE property_name = 'rating'
GROUP BY zone;
```

Attributes that change over time:

```sql
SELECT prim_path, property_name, time_sample_count
FROM usd_properties('facility.usd')
WHERE time_sample_count > 0;
```

Large arrays (mesh points, normals, ...) are counted but not returned by default:

```sql
SELECT prim_path, sum(array_length) AS points
FROM usd_properties('facility.usd')
WHERE property_name = 'points'
GROUP BY prim_path
ORDER BY points DESC
LIMIT 10;
```

## Spatial queries

Positions in meters, whatever the stage's units:

```sql
SELECT x.prim_path, x.x * m.meters_per_unit AS x_m, x.y * m.meters_per_unit AS y_m, x.z * m.meters_per_unit AS z_m
FROM usd_xforms('facility.usd') x, usd_stage_metadata('facility.usd') m;
```

Everything inside a box:

```sql
SELECT prim_path
FROM usd_bounds('facility.usd')
WHERE min_x >= 0 AND max_x <= 1000
  AND min_y >= 0 AND max_y <= 500;
```

The nearest neighbour of each unit:

```sql
WITH units AS (
    SELECT x.prim_path, x.x, x.y, x.z
    FROM usd_xforms('facility.usd') x
    JOIN usd_prims('facility.usd') p USING (prim_path)
    WHERE p.kind = 'component'
)
SELECT a.prim_path, b.prim_path AS nearest,
       sqrt((a.x - b.x) ** 2 + (a.y - b.y) ** 2 + (a.z - b.z) ** 2) AS distance
FROM units a, units b
WHERE a.prim_path <> b.prim_path
QUALIFY row_number() OVER (PARTITION BY a.prim_path ORDER BY distance) = 1;
```

Overlapping bounding boxes (possible clashes) among components:

```sql
WITH boxes AS (
    SELECT b.*
    FROM usd_bounds('facility.usd') b
    JOIN usd_prims('facility.usd') p USING (prim_path)
    WHERE p.kind = 'component' AND b.min_x IS NOT NULL
)
SELECT a.prim_path, b.prim_path
FROM boxes a JOIN boxes b
  ON a.prim_path < b.prim_path
 AND a.min_x < b.max_x AND b.min_x < a.max_x
 AND a.min_y < b.max_y AND b.min_y < a.max_y
 AND a.min_z < b.max_z AND b.min_z < a.max_z;
```

Prims that are rotated or scaled:

```sql
SELECT prim_path, rotation, scale FROM usd_xforms('facility.usd') WHERE has_rotation OR has_scale;
```

The footprint of each zone:

```sql
SELECT prim_path, max_x - min_x AS width, max_y - min_y AS depth
FROM usd_bounds('facility.usd')
WHERE prim_path SIMILAR TO '/Site/[^/]+';
```

## Maps and GIS

DuckDB's [spatial extension](https://duckdb.org/docs/stable/core_extensions/spatial/overview) turns positions and
bounds into geometries that GIS tools read:

```sql
INSTALL spatial;
LOAD spatial;
-- longitude before latitude (the default from DuckDB 2.1)
SET geometry_always_xy = true;
```

The footprint of each unit, in meters, and the floor area the units of each zone take. The ground of a Z-up stage
is its x/y plane; for a Y-up stage, use `-max_z` and `-min_z` in place of `min_y` and `max_y`. Bounds are
axis-aligned, so the footprint of a rotated unit is the rectangle around it.

```sql
CREATE TABLE footprints AS
SELECT b.prim_path, split_part(b.prim_path, '/', 3) AS zone,
       ST_MakeEnvelope(b.min_x * m.meters_per_unit, b.min_y * m.meters_per_unit,
                       b.max_x * m.meters_per_unit, b.max_y * m.meters_per_unit) AS footprint
FROM usd_bounds('facility.usd') b
JOIN usd_prims('facility.usd') p USING (prim_path)
CROSS JOIN usd_stage_metadata('facility.usd') m
WHERE p.kind = 'component' AND b.min_x IS NOT NULL;

SELECT zone, count(*) AS units, sum(ST_Area(footprint)) AS floor_area_m2
FROM footprints
GROUP BY zone;
```

USD does not record where a scene is on Earth. Given where the stage's origin lies in a projected coordinate system
(here easting 500000 and northing 5000000 in UTM zone 32N), and with the stage's x axis pointing east, a macro
places geometries in longitude and latitude:

```sql
CREATE MACRO to_wgs84(geom) AS
    ST_Transform(ST_Translate(geom, 500000, 5000000), 'EPSG:32632', 'EPSG:4326');

SELECT x.prim_path, to_wgs84(ST_Point(x.x * m.meters_per_unit, x.y * m.meters_per_unit)) AS location
FROM usd_xforms('facility.usd') x
JOIN usd_prims('facility.usd') p USING (prim_path)
CROSS JOIN usd_stage_metadata('facility.usd') m
WHERE p.kind = 'component';
```

The footprints as a GeoPackage, for QGIS or ArcGIS (`DRIVER 'GeoJSON'` writes a file for web maps):

```sql
COPY (SELECT prim_path, zone, to_wgs84(footprint) AS geom FROM footprints)
TO 'units.gpkg' WITH (FORMAT GDAL, DRIVER 'GPKG', SRS 'EPSG:4326');
```

The units that stand in an area drawn in a GIS tool:

```sql
SELECT f.prim_path, z.name
FROM footprints f
JOIN ST_Read('flood_zone.geojson') z ON ST_Intersects(to_wgs84(f.footprint), z.geom);
```

## Graphs of relationships

Which prims depend on each target, and how much:

```sql
SELECT target_path, relationship_name, count(*) AS dependents
FROM usd_relationships('facility.usd')
GROUP BY ALL
ORDER BY dependents DESC;
```

Targets that do not exist (dangling relationships):

```sql
SELECT r.prim_path, r.relationship_name, r.target_path
FROM usd_relationships('facility.usd') r
ANTI JOIN usd_prims('facility.usd', include_abstract := true) p
  ON split_part(r.target_path, '.', 1) = p.prim_path;
```

Everything downstream of one prim, following a relationship transitively (`UNION` drops the prims already
reached, so the query also ends when the connections form a cycle):

```sql
WITH RECURSIVE edges AS (
    SELECT prim_path AS source, target_path AS target
    FROM usd_relationships('facility.usd')
    WHERE relationship_name = 'connection'
),
reach(prim_path) AS (
    SELECT '/Site/Zone_A/Unit_001'
    UNION
    SELECT e.source FROM reach r JOIN edges e ON e.target = r.prim_path
)
SELECT * FROM reach ORDER BY prim_path;
```

Material bindings:

```sql
SELECT prim_path, target_path AS material
FROM usd_relationships('facility.usd')
WHERE relationship_name = 'material:binding';
```

## Materials

```sql
-- how many prims use each material (counting every instance)
SELECT material_path, count(*) AS prims
FROM usd_material_bindings('facility.usd', instance_proxies := true)
GROUP BY ALL
ORDER BY prims DESC;

-- bindings to materials that do not exist
SELECT prim_path, material_path FROM usd_material_bindings('facility.usd') WHERE NOT material_found;

-- meshes without a material
SELECT p.prim_path
FROM usd_prims('facility.usd') p
ANTI JOIN usd_material_bindings('facility.usd') b USING (prim_path)
WHERE p.prim_type = 'Mesh';

-- materials no prim uses (with the all-purpose bindings)
SELECT m.prim_path
FROM usd_prims('facility.usd') m
ANTI JOIN usd_material_bindings('facility.usd', instance_proxies := true) b ON b.material_path = m.prim_path
WHERE m.prim_type = 'Material';
```

## Moving things and recorded data

These examples use a yard of vehicles, `/Yard/Vehicles/<vehicle>`, whose positions and a custom `speed` attribute
are recorded as time samples.

What changes over time, and over which time range:

```sql
SELECT prim_path, property_name, count(*) AS samples, min(time) AS first, max(time) AS last
FROM usd_time_samples('yard.usd')
GROUP BY ALL
ORDER BY samples DESC;
```

Recorded values, with times in seconds:

```sql
SELECT s.prim_path, s.time / m.time_codes_per_second AS seconds, s.value::DOUBLE AS speed
FROM usd_time_samples('yard.usd') s, usd_stage_metadata('yard.usd') m
WHERE s.property_name = 'speed';
```

When each vehicle first went faster than 4:

```sql
SELECT prim_path, min(time) AS first_time
FROM usd_time_samples('yard.usd')
WHERE property_name = 'speed' AND value::DOUBLE > 4
GROUP BY prim_path;
```

The distance each vehicle covered, from its position every second (24 time codes per second):

```sql
WITH path AS (
    SELECT prim_path, time, x, y
    FROM usd_xforms('yard.usd', root := '/Yard/Vehicles', times := range(0, 241, 24))
    WHERE prim_path SIMILAR TO '/Yard/Vehicles/[^/]+'
)
SELECT prim_path, sum(sqrt((x - prev_x) ** 2 + (y - prev_y) ** 2)) AS distance
FROM (
    SELECT *, lag(x) OVER w AS prev_x, lag(y) OVER w AS prev_y
    FROM path
    WINDOW w AS (PARTITION BY prim_path ORDER BY time)
)
GROUP BY prim_path;
```

Vehicles that came within 5 units of each other, and when first (checked every quarter second: sample finely
enough not to miss a close pass between two times):

```sql
WITH positions AS (
    SELECT prim_path, time, x, y
    FROM usd_xforms('yard.usd', root := '/Yard/Vehicles', times := range(0, 241, 6))
    WHERE prim_path SIMILAR TO '/Yard/Vehicles/[^/]+'
)
SELECT a.prim_path, b.prim_path AS other, min(a.time) AS first_time
FROM positions a
JOIN positions b ON a.time = b.time AND a.prim_path < b.prim_path
WHERE sqrt((a.x - b.x) ** 2 + (a.y - b.y) ** 2) < 5
GROUP BY ALL;
```

## Robots and physics

These examples use a robot cell, `/Cell/<robot>/<body or joint>`, described with OpenUSD's physics schemas.

Rigid bodies and their mass:

```sql
SELECT p.prim_path, q.value::DOUBLE AS mass
FROM usd_prims('cell.usd') p
LEFT JOIN usd_properties('cell.usd') q ON q.prim_path = p.prim_path AND q.property_name = 'physics:mass'
WHERE list_contains(p.applied_schemas, 'PhysicsRigidBodyAPI');
```

Joints, the bodies they connect, and their limits:

```sql
SELECT r.prim_path AS joint, p.prim_type,
       max(r.target_path) FILTER (r.relationship_name = 'physics:body0') AS body0,
       max(r.target_path) FILTER (r.relationship_name = 'physics:body1') AS body1
FROM usd_relationships('cell.usd') r
JOIN usd_prims('cell.usd') p USING (prim_path)
WHERE r.relationship_name IN ('physics:body0', 'physics:body1')
GROUP BY ALL;

PIVOT (
    SELECT prim_path, property_name, value::DOUBLE AS degrees
    FROM usd_properties('cell.usd', authored_only := true)
    WHERE property_name IN ('physics:lowerLimit', 'physics:upperLimit')
)
ON property_name USING first(degrees);
```

The bodies each base body moves, following the joints from `body0` to `body1` (`UNION` ends on cycles):

```sql
WITH RECURSIVE joints AS (
    SELECT max(target_path) FILTER (relationship_name = 'physics:body0') AS parent,
           max(target_path) FILTER (relationship_name = 'physics:body1') AS child
    FROM usd_relationships('cell.usd')
    WHERE relationship_name IN ('physics:body0', 'physics:body1')
    GROUP BY prim_path
),
moved(base, body) AS (
    SELECT parent, child FROM joints WHERE parent NOT IN (SELECT child FROM joints)
    UNION
    SELECT moved.base, joints.child FROM moved JOIN joints ON joints.parent = moved.body
)
SELECT base, list(body ORDER BY body) AS moves
FROM moved
GROUP BY base;
```

## Semantic labels

Labels authored with OpenUSD's semantics schema are `semantics:labels:<taxonomy>` attributes. How many prims carry
each label:

```sql
WITH labels AS (
    SELECT prim_path, replace(property_name, 'semantics:labels:', '') AS taxonomy, value::VARCHAR[] AS labels
    FROM usd_properties('cell.usd', authored_only := true)
    WHERE property_name LIKE 'semantics:labels:%'
)
SELECT taxonomy, label, count(*) AS prims
FROM (SELECT taxonomy, unnest(labels) AS label FROM labels)
GROUP BY ALL
ORDER BY prims DESC;
```

A label applies to the prim's descendants too: the prims under each label of the `class` taxonomy:

```sql
WITH labeled AS (
    SELECT prim_path, unnest(value::VARCHAR[]) AS label
    FROM usd_properties('cell.usd', authored_only := true)
    WHERE property_name = 'semantics:labels:class'
)
SELECT l.label, count(*) AS prims
FROM labeled l
JOIN usd_prims('cell.usd') p ON p.prim_path = l.prim_path OR starts_with(p.prim_path, l.prim_path || '/')
GROUP BY ALL;
```

## Characters

Skeletons and their number of joints:

```sql
SELECT s.prim_path, s.array_length AS joints
FROM usd_properties('crowd.usd', authored_only := true) s
JOIN usd_prims('crowd.usd') p USING (prim_path)
WHERE p.prim_type = 'Skeleton' AND s.property_name = 'joints';
```

Skeleton bindings (on a skinned mesh, or on an ancestor such as the `SkelRoot`):

```sql
SELECT prim_path, target_path AS skeleton
FROM usd_relationships('crowd.usd')
WHERE relationship_name = 'skel:skeleton';
```

## Composition health

```sql
-- broken references, payloads and sublayers
SELECT error_type, prim_path, message FROM usd_composition_errors('facility.usd');

-- every file the stage pulls in
SELECT layer_identifier, format, resolved_path FROM usd_layers('facility.usd');

-- variant selections across the scene
SELECT variant_set, selection, count(*) FROM usd_variants('facility.usd') GROUP BY ALL;

-- which asset files the scene uses, and how many times (counting every instance)
SELECT asset_path, count(*) AS uses
FROM usd_composition_arcs('facility.usd', instance_proxies := true)
WHERE arc_type IN ('reference', 'payload') AND asset_path IS NOT NULL
GROUP BY ALL
ORDER BY uses DESC;

-- the prims that use a given asset
SELECT prim_path, arc_type
FROM usd_composition_arcs('facility.usd')
WHERE asset_path LIKE '%unit.usd';
```

## Many files

Scan a directory of scenes at once and keep track of where each row came from:

```sql
SELECT filename, count(*) AS prims
FROM usd_prims('scenes/*.usd', filename := true)
GROUP BY filename;

SELECT filename, up_axis, meters_per_unit
FROM usd_stage_metadata('scenes/*.usd', filename := true);
```

Compare two versions of a scene:

```sql
SELECT coalesce(a.prim_path, b.prim_path) AS prim_path,
       CASE WHEN a.prim_path IS NULL THEN 'added' WHEN b.prim_path IS NULL THEN 'removed' ELSE 'moved' END AS change
FROM usd_xforms('before.usd') a
FULL OUTER JOIN usd_xforms('after.usd') b USING (prim_path)
WHERE a.prim_path IS NULL OR b.prim_path IS NULL
   OR abs(a.x - b.x) + abs(a.y - b.y) + abs(a.z - b.z) > 0.001;
```

Save an extract for other tools:

```sql
COPY (SELECT * EXCLUDE (custom_data, asset_info) FROM usd_prims('facility.usd')) TO 'prims.parquet';
COPY (SELECT prim_path, x, y, z FROM usd_xforms('facility.usd')) TO 'positions.csv';
```

## Large scenes

- Select only the columns you need: most columns are computed only when a query selects them (a property's
  `value`, for example).
- Restrict the scan with `root := '/Site/Zone_A'`: only that subtree is composed.
- `load_payloads := false` composes the stage without its payloads, which is much faster when the payloads hold
  heavy geometry you do not need.
- `instance_proxies := true` can multiply the number of rows many times over; leave it off unless you need the
  contents of instances.
- `authored_only := true` drops the schema fallback properties, which are most of the rows of `usd_properties`.
- Opening a large stage takes a few seconds (USD composes it). The composed stage stays in memory, so the queries
  that follow on the same file, with the same `root` and `load_payloads`, skip that step (see
  [Stage cache](functions.md#stage-cache)).
