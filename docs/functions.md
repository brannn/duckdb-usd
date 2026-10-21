# Function reference

- [Common arguments and options](#common-arguments-and-options)
- [usd_prims](#usd_prims)
- [usd_properties](#usd_properties)
- [usd_relationships](#usd_relationships)
- [usd_xforms](#usd_xforms)
- [usd_bounds](#usd_bounds)
- [usd_variants](#usd_variants)
- [usd_layers](#usd_layers)
- [usd_stage_metadata](#usd_stage_metadata)
- [usd_composition_errors](#usd_composition_errors)
- [usd_composition_arcs](#usd_composition_arcs)
- [usd_material_bindings](#usd_material_bindings)
- [usd_time_samples](#usd_time_samples)
- [usd_version](#usd_version)
- [Values and VARIANT](#values-and-variant)
- [Stage cache](#stage-cache)
- [Errors](#errors)

## Common arguments and options

Every table function takes the stage to read as its first argument:

```sql
FROM usd_prims('scene.usda');                         -- a file (.usd, .usda, .usdc or .usdz)
FROM usd_prims('scenes/*.usd');                       -- a glob
FROM usd_prims(['part_1.usda', 'part_2.usda']);       -- a list of files and globs
FROM 'scene.usda';                                    -- shorthand for usd_prims('scene.usda')
```

Each file is opened as the root layer of a stage: sublayers, references and payloads are composed as USD would.
Files are read in order, so results keep the order of the list. Relative paths are looked up in DuckDB's
`file_search_path` as well.

Named options shared by all table functions:

| Option | Type | Default | Meaning |
|---|---|---|---|
| `root` | `VARCHAR` | `'/'` | Path of the prim whose subtree is scanned, including the prim itself. Only that subtree is composed (a USD population mask), which makes scans of a part of a large scene much faster. A root that does not exist in a file contributes no rows. |
| `instance_proxies` | `BOOLEAN` | `false` | Visit the prims inside instances (instance proxies). Instances themselves are always visited. |
| `active_only` | `BOOLEAN` | `false` | Skip inactive prims. By default an inactive prim is listed (its children are never composed). |
| `include_abstract` | `BOOLEAN` | `false` | Visit abstract prims (`class` specifier) and their descendants. |
| `include_undefined` | `BOOLEAN` | `false` | Visit undefined prims (only `over` opinions). |
| `load_payloads` | `BOOLEAN` | `true` | Load payloads. With `false`, prims under payloads are not composed. |
| `filename` | `BOOLEAN` | `false` | Add a `filename` column with the path of the file each row comes from. |
| `ignore_errors` | `BOOLEAN` | `false` | Skip files that cannot be read (missing, not USD files, or failing to open, e.g. to parse) instead of failing the query. Skipped files are logged as warnings. |

Prims are visited in depth-first order, the order USD traverses a stage.

### Time

[usd_properties](#usd_properties), [usd_xforms](#usd_xforms) and [usd_bounds](#usd_bounds) read values, transforms
and bounds at a time code:

| Option | Type | Default | Meaning |
|---|---|---|---|
| `time` | `DOUBLE` | default time | Time code at which values, transforms and bounds are read. Without it, attributes that only have time samples have no value. |
| `times` | `DOUBLE[]` | | Read at each of these time codes: one row per prim (or property) and time, with a `time` column after the function's columns. Cannot be combined with `time`. |

Between time samples, values are interpolated as USD does: floating point values, vectors and matrices linearly (by
default), spline-animated attributes along their spline; values that cannot be interpolated, such as strings, hold
the previous sample. With `times`, each batch of prims is listed time by time: use `ORDER BY prim_path, time` to
follow each prim through time. [usd_time_samples](#usd_time_samples) lists the times and values actually authored.

```sql
-- where each vehicle is every second (24 time codes per second)
SELECT prim_path, time, x, y
FROM usd_xforms('yard.usd', root := '/Yard/Vehicles', times := range(0, 241, 24))
ORDER BY prim_path, time;
```

## usd_prims

One row per prim.

| Column | Type | Description |
|---|---|---|
| `prim_path` | `VARCHAR` | Path of the prim, e.g. `/Site/Zone_A/Unit_001` |
| `parent_path` | `VARCHAR` | Path of the parent (`/` for top-level prims) |
| `name` | `VARCHAR` | Last element of the path |
| `depth` | `INTEGER` | Number of path elements (1 for top-level prims) |
| `prim_type` | `VARCHAR` | Schema type name (`Xform`, `Mesh`, ...), `NULL` for prims without a type |
| `kind` | `VARCHAR` | Model kind (`assembly`, `group`, `component`, `subcomponent`), `NULL` if not set |
| `specifier` | `VARCHAR` | `def`, `over` or `class` |
| `active` | `BOOLEAN` | Whether the prim is active |
| `defined` | `BOOLEAN` | Whether the prim is defined (has a `def` opinion) |
| `abstract` | `BOOLEAN` | Whether the prim is abstract (a `class` or inside one) |
| `instanceable` | `BOOLEAN` | Whether the prim is marked instanceable |
| `instance` | `BOOLEAN` | Whether the prim is an instance |
| `instance_proxy` | `BOOLEAN` | Whether the prim is inside an instance (only with `instance_proxies := true`) |
| `has_payload` | `BOOLEAN` | Whether the prim has authored payloads |
| `loaded` | `BOOLEAN` | Whether the prim's payloads are loaded |
| `applied_schemas` | `VARCHAR[]` | Applied API schemas, e.g. `[MaterialBindingAPI]` |
| `custom_data` | `VARIANT` | The prim's authored `customData` dictionary, `NULL` if none |
| `asset_info` | `VARIANT` | The prim's authored `assetInfo` dictionary, `NULL` if none |

```sql
SELECT kind, count(*) FROM usd_prims('scene.usda') GROUP BY ALL;
SELECT prim_path FROM usd_prims('scene.usda') WHERE NOT active;
SELECT prim_path, custom_data.owner::VARCHAR FROM usd_prims('scene.usda') WHERE custom_data IS NOT NULL;
```

## usd_properties

One row per property (attribute or relationship) of each prim, in name order. Properties that a prim's schema
defines exist even when nothing is authored; `is_authored` tells them apart.

| Column | Type | Description |
|---|---|---|
| `prim_path` | `VARCHAR` | Path of the prim that owns the property |
| `property_name` | `VARCHAR` | Name, including namespaces (`xformOp:translate`, `primvars:displayColor`) |
| `property_kind` | `VARCHAR` | `attribute` or `relationship` |
| `type_name` | `VARCHAR` | Value type of an attribute (`double`, `float3[]`, `token`, ...), `NULL` for relationships |
| `is_array` | `BOOLEAN` | Whether the attribute is array-valued, `NULL` for relationships |
| `is_custom` | `BOOLEAN` | Whether the property is custom (not defined by a schema) |
| `is_authored` | `BOOLEAN` | Whether the property has authored opinions |
| `variability` | `VARCHAR` | `varying` or `uniform` (relationships are always `uniform`) |
| `time_sample_count` | `BIGINT` | Number of time samples of an attribute, `NULL` for relationships |
| `array_length` | `BIGINT` | Number of elements of an array value, or of a relationship's targets |
| `value` | `VARIANT` | The value at the default time (or at `time`, or each of `times`); for a relationship, the list of its targets. `NULL` if there is no value, or if the array is longer than `max_array_length`. |

Options:

| Option | Type | Default | Meaning |
|---|---|---|---|
| `authored_only` | `BOOLEAN` | `false` | Skip properties without authored opinions (schema fallbacks) |
| `max_array_length` | `BIGINT` | `1000` | Longest array returned in `value`; longer arrays (e.g. mesh points) have a `NULL` value. `0` means unlimited. |

Values are only read when `value` or `array_length` is selected. A `WHERE` condition that compares `property_name`
with constants (`=`, `IN`) is applied while scanning: only those properties are read, and the rest of the query only
sees their values.

```sql
SELECT prim_path, value::DOUBLE AS rating
FROM usd_properties('scene.usda')
WHERE property_name = 'rating';

SELECT property_name, value::DOUBLE[] FROM usd_properties('scene.usda', root := '/Site/Zone_A/Unit_001')
WHERE type_name LIKE '%3';
```

## usd_relationships

One row per relationship target. Relationships without targets produce no rows.

| Column | Type | Description |
|---|---|---|
| `prim_path` | `VARCHAR` | Path of the prim that owns the relationship |
| `relationship_name` | `VARCHAR` | Name of the relationship |
| `target_path` | `VARCHAR` | Absolute path of the target (a prim or a property) |
| `target_index` | `INTEGER` | Position of the target in the relationship (from 0) |

Options:

| Option | Type | Default | Meaning |
|---|---|---|---|
| `forwarded` | `BOOLEAN` | `false` | Report forwarded targets: a target that is itself a relationship is replaced by that relationship's targets |

```sql
SELECT target_path, count(*) AS dependents
FROM usd_relationships('scene.usda')
GROUP BY ALL ORDER BY dependents DESC;
```

## usd_xforms

One row per xformable prim (`Xform`, geometry, cameras, lights, ...).

| Column | Type | Description |
|---|---|---|
| `prim_path` | `VARCHAR` | Path of the prim |
| `x`, `y`, `z` | `DOUBLE` | World-space position (the translation of the local-to-world transform), in stage units |
| `rotation` | `DOUBLE[3]` | World-space rotation as XYZ Euler angles in degrees (the convention of `xformOp:rotateXYZ`) |
| `scale` | `DOUBLE[3]` | World-space scale |
| `has_rotation` | `BOOLEAN` | Whether the world transform rotates |
| `has_scale` | `BOOLEAN` | Whether the world transform scales |
| `world_matrix` | `DOUBLE[16]` | Local-to-world matrix, row-major (USD convention: the translation is in elements 13 to 15) |
| `local_matrix` | `DOUBLE[16]` | The prim's own transform, relative to its parent |
| `resets_xform_stack` | `BOOLEAN` | Whether the prim ignores its ancestors' transforms (`!resetXformStack!`) |
| `xform_op_order` | `VARCHAR[]` | The prim's `xformOpOrder` |
| `time_varying` | `BOOLEAN` | Whether the prim's transform may change over time |

Rotation and scale are decomposed from the world matrix and rounded to 9 decimal places. Coordinates are in stage
units: multiply by `meters_per_unit` from [usd_stage_metadata](#usd_stage_metadata) to get meters.

```sql
SELECT prim_path, x, y, z FROM usd_xforms('scene.usda', root := '/Site/Zone_A');
SELECT prim_path, rotation FROM usd_xforms('scene.usda') WHERE has_rotation;
```

## usd_bounds

One row per imageable prim, with its world-space axis-aligned bounding box. The box covers the prim's geometry and
that of its descendants, including the prototypes of instances; prims without geometry have `NULL` bounds.

| Column | Type | Description |
|---|---|---|
| `prim_path` | `VARCHAR` | Path of the prim |
| `min_x`, `min_y`, `min_z` | `DOUBLE` | Minimum corner, in stage units |
| `max_x`, `max_y`, `max_z` | `DOUBLE` | Maximum corner, in stage units |

Options:

| Option | Type | Default | Meaning |
|---|---|---|---|
| `purposes` | `VARCHAR[]` | `['default', 'render']` | Purposes of the geometry to include: `default`, `render`, `proxy`, `guide` |
| `use_extents_hint` | `BOOLEAN` | `true` | Use authored `extentsHint` instead of visiting descendants |

```sql
SELECT prim_path, max_x - min_x AS width, max_y - min_y AS depth, max_z - min_z AS height
FROM usd_bounds('scene.usda')
WHERE min_x IS NOT NULL;
```

## usd_variants

One row per variant set of each prim.

| Column | Type | Description |
|---|---|---|
| `prim_path` | `VARCHAR` | Path of the prim |
| `variant_set` | `VARCHAR` | Name of the variant set |
| `selection` | `VARCHAR` | Selected variant, `NULL` if none |
| `variants` | `VARCHAR[]` | Available variants |

## usd_layers

One row per layer used by the stage: the root layer first, then its sublayers and every layer brought in by
references and payloads, by identifier.

| Column | Type | Description |
|---|---|---|
| `layer_identifier` | `VARCHAR` | Identifier of the layer |
| `resolved_path` | `VARCHAR` | Path of the file the layer was read from |
| `format` | `VARCHAR` | File format (`usda`, `usdc`, `usdz`, ...) |
| `is_root` | `BOOLEAN` | Whether this is the stage's root layer |
| `is_anonymous` | `BOOLEAN` | Whether the layer exists only in memory |
| `sublayers` | `VARCHAR[]` | Sublayer paths authored in the layer |
| `default_prim` | `VARCHAR` | Default prim authored in the layer |
| `documentation` | `VARCHAR` | The layer's documentation string |

## usd_stage_metadata

One row per stage.

| Column | Type | Description |
|---|---|---|
| `default_prim` | `VARCHAR` | The root layer's default prim |
| `up_axis` | `VARCHAR` | `Y` or `Z` (USD's fallback is `Y`) |
| `meters_per_unit` | `DOUBLE` | Length of a stage unit in meters (USD's fallback is 0.01) |
| `start_time_code`, `end_time_code` | `DOUBLE` | Authored time code range, `NULL` if not authored |
| `time_codes_per_second` | `DOUBLE` | |
| `frames_per_second` | `DOUBLE` | |
| `layer_count` | `BIGINT` | Number of layers the stage uses |
| `documentation` | `VARCHAR` | The root layer's documentation string |
| `custom_layer_data` | `VARIANT` | The root layer's `customLayerData` dictionary |

## usd_composition_errors

One row per error USD encountered while composing the stage — for example a reference to a file that does not
exist.

| Column | Type | Description |
|---|---|---|
| `error_type` | `VARCHAR` | The kind of error, e.g. `InvalidAssetPath`, `UnresolvedPrimPath`, `ArcCycle` |
| `prim_path` | `VARCHAR` | Path of the prim whose composition failed |
| `message` | `VARCHAR` | USD's description of the error |

When any other function opens a stage that has composition errors, DuckDB logs a single warning that points here.

## usd_composition_arcs

One row per composition arc of each prim — each reference, payload, inherit, specialize and variant selection that
contributes to it — in strength order. It answers which assets a scene uses, where, and how often.

| Column | Type | Description |
|---|---|---|
| `prim_path` | `VARCHAR` | Path of the prim |
| `arc_index` | `INTEGER` | Position of the arc among the prim's arcs listed here, strongest first (0) |
| `arc_type` | `VARCHAR` | `reference`, `payload`, `inherit`, `specialize`, `variant` or `relocate` |
| `asset_path` | `VARCHAR` | The asset path as authored, for references and payloads to another file. `NULL` for references within the same layer stack and for other arc types |
| `target_layer` | `VARCHAR` | Identifier of the layer the arc's opinions come from |
| `target_prim_path` | `VARCHAR` | Path of the prim the arc targets, in that layer's namespace (with the selection for variant arcs, e.g. `/Prim{size=large}`) |
| `variant_set`, `variant` | `VARCHAR` | The variant set and selected variant, for variant arcs |
| `introducing_layer` | `VARCHAR` | Identifier of the layer in which the arc is authored |
| `introducing_prim_path` | `VARCHAR` | Path of the prim on which the arc is authored, in that layer's namespace |
| `is_ancestral` | `BOOLEAN` | Whether the prim gets the arc through an ancestor (see `include_ancestral`) |
| `is_implicit` | `BOOLEAN` | Whether the arc is implied by another arc rather than authored, e.g. an inherit that a referenced asset carries into the referencing layer stack |
| `has_specs` | `BOOLEAN` | Whether the arc's target provides any opinions (an inherit of a class nobody defines does not) |

| Option | Type | Default | Meaning |
|---|---|---|---|
| `include_ancestral` | `BOOLEAN` | `false` | Also list the arcs a prim gets through its ancestors: every descendant of a referenced prim is composed through its ancestor's reference. Much slower on large scenes, since every prim is analyzed. |

References to files that cannot be opened have no arc; they are reported by `usd_composition_errors`. Unloaded
payloads (`load_payloads := false`) are not composed and have no arc either. With `instance_proxies := true`, the
prims inside instances report the arcs of the shared prototype, with paths on the instance itself mapped to the
instance they are in.

```sql
-- The assets a scene references, and how many times each is used
SELECT asset_path, count(*) AS uses
FROM usd_composition_arcs('scene.usd', instance_proxies := true)
WHERE arc_type IN ('reference', 'payload') AND asset_path IS NOT NULL
GROUP BY ALL
ORDER BY uses DESC;
```

## usd_material_bindings

One row per prim that is bound to a material: the material USD resolves for it, and the binding that decides it —
authored on the prim itself or inherited from an ancestor, direct or through a collection. Only imageable prims
(geometry, transforms, scopes) and geometry subsets are listed; materials, shaders and other prims that are not
rendered are not.

| Column | Type | Description |
|---|---|---|
| `prim_path` | `VARCHAR` | Path of the prim |
| `material_path` | `VARCHAR` | Path of the bound material (the binding's target, whether or not a material is there) |
| `material_found` | `BOOLEAN` | Whether `material_path` is a material on the stage: `false` for a binding to a missing prim or to a prim that is not a material |
| `binding_type` | `VARCHAR` | `direct` (`material:binding`) or `collection` (a binding of a collection of prims) |
| `collection_path` | `VARCHAR` | The bound collection, e.g. `/World.collection:metal`, for collection bindings |
| `binding_prim_path` | `VARCHAR` | Path of the prim on which the deciding binding is authored |
| `inherited` | `BOOLEAN` | Whether the binding is authored on an ancestor rather than on the prim itself |
| `strength` | `VARCHAR` | `weakerThanDescendants` (the default) or `strongerThanDescendants` (an ancestor's binding that overrides the prim's own) |
| `material_purpose` | `VARCHAR` | Purpose of the deciding binding: `allPurpose`, `full` or `preview` |

| Option | Type | Default | Meaning |
|---|---|---|---|
| `material_purpose` | `VARCHAR` | `'allPurpose'` | Resolve bindings for this purpose: `'allPurpose'`, `'full'` (final renders) or `'preview'` (viewports). A purpose-specific binding wins where there is one; elsewhere the all-purpose binding applies. |

Bindings authored without `MaterialBindingAPI` applied to the prim (older assets) are honored, as by USD. With
`instance_proxies := true`, the prims inside instances are listed, and bindings inside the instance are reported
on the instance proxy that holds them.

```sql
-- How many prims use each material
SELECT material_path, count(*) AS prims
FROM usd_material_bindings('scene.usd', instance_proxies := true)
GROUP BY ALL
ORDER BY prims DESC;
```

## usd_time_samples

One row per authored time sample of each attribute: the recorded values of animated transforms, sensor readings,
simulation results and other data that changes over time. Each attribute's samples are listed in time order.
Attributes animated with a spline list their knots, and samples read from value clips are included. As in USD's value
resolution, a default value authored in a stronger layer hides the time samples of weaker ones.

| Column | Type | Description |
|---|---|---|
| `prim_path` | `VARCHAR` | Path of the prim that owns the attribute |
| `property_name` | `VARCHAR` | Name of the attribute |
| `type_name` | `VARCHAR` | Value type (`double`, `double3`, `string[]`, ...) |
| `time` | `DOUBLE` | Time code of the sample (or knot) |
| `array_length` | `BIGINT` | Number of elements of an array value |
| `value` | `VARIANT` | The value at that time. `NULL` for a blocked sample (`None`), or if the array is longer than `max_array_length`. |

| Option | Type | Default | Meaning |
|---|---|---|---|
| `start_time` | `DOUBLE` | | List only the samples at or after this time code |
| `end_time` | `DOUBLE` | | List only the samples at or before this time code |
| `max_array_length` | `BIGINT` | `1000` | Longest array returned in `value`; longer arrays have a `NULL` value. `0` means unlimited. |

Values are only read when `value` or `array_length` is selected. As in `usd_properties`, a `WHERE` condition that
compares `property_name` with constants (`=`, `IN`) is applied while scanning. Time codes convert to seconds with
`time_codes_per_second` from [usd_stage_metadata](#usd_stage_metadata). To read values at times of your choice
(between samples, or on a common grid for several attributes), use the `times` option of
[usd_properties](#usd_properties) instead.

```sql
-- the recorded speed of each vehicle
SELECT prim_path, time, value::DOUBLE AS speed
FROM usd_time_samples('yard.usd')
WHERE property_name = 'speed'
ORDER BY prim_path, time;
```

## usd_version

`usd_version()` returns the version of OpenUSD the extension was built with, e.g. `26.05`.

## Values and VARIANT

`usd_properties.value`, `usd_time_samples.value`, `usd_prims.custom_data`, `usd_prims.asset_info` and
`usd_stage_metadata.custom_layer_data` are DuckDB `VARIANT`s. They keep the type of the USD value:

| USD | VARIANT holds |
|---|---|
| `bool` | boolean |
| integer types | integer |
| `half`, `float`, `double`, `timecode` | double |
| `string`, `token`, `asset` (the authored path) | string |
| vectors (`double3`, `color3f`, ...) | list of numbers |
| matrices | list of rows |
| quaternions | `{real, imaginary}` |
| arrays | list |
| dictionaries | object |

Cast them to the type you need, or extract fields:

```sql
SELECT prim_path, value::DOUBLE FROM usd_properties('scene.usda') WHERE property_name = 'rating';
SELECT prim_path, (value::DOUBLE[][])[4] AS last_row FROM usd_properties('scene.usda') WHERE type_name = 'matrix4d';
SELECT custom_data.owner::VARCHAR FROM usd_prims('scene.usda');
```

Casting a value to a type it cannot be converted to (e.g. a string to `DOUBLE`) is an error. A condition on
`property_name` (`=`, `IN`) is applied before the rest of the query sees any value, so
`WHERE property_name = 'rating' AND value::DOUBLE > 4` is safe. Other conditions, such as one on `type_name`, give no
such guarantee (DuckDB evaluates the conditions of a `WHERE` clause in the order it estimates is fastest): use
`TRY_CAST` with them.

Non-finite floating point numbers (NaN, infinity) become `NULL`. USD strings are not required to be valid UTF-8:
in strings that are not, here and in `VARCHAR` columns, the invalid bytes are replaced with `?`.

## Stage cache

Opening a stage is usually most of a query's time: USD reads every layer and composes every prim. Opened stages stay
in memory between queries, so later queries on the same file (with the same `root` and `load_payloads`) start
immediately. Every query that uses a cached stage checks it first:

- if any file the stage read has changed (its modification time or size), or a reference, payload or sublayer that
  was missing now exists, the stage is opened again;
- the stage's files are checked against DuckDB's file access settings again;
- composition errors are reported again (as a warning in DuckDB's log).

Cached stages are kept in DuckDB's object cache and count toward `memory_limit`, by an estimate of a few kilobytes per
prim (shown as `OBJECT_CACHE` in `duckdb_memory()`). Together they take at most half of `memory_limit`: when a newly
opened stage does not fit, the least recently used ones are dropped, and a stage that alone needs more than half of
`memory_limit` is not cached. DuckDB also drops cached stages when a query needs the memory.

| Setting | Type | Default | Meaning |
|---|---|---|---|
| `usd_stage_cache` | `BOOLEAN` | `true` | Keep opened stages between queries. Setting it to `false` also frees the stages already cached. |

```sql
SET usd_stage_cache = false;   -- free the cached stages, and stop caching
```

## Errors

| Situation | Error |
|---|---|
| The file does not exist | `IO Error: USD file not found` |
| A glob matches nothing | `IO Error: no USD files found` |
| The file is not a USD file | `Invalid Input Error: Not a USD file` |
| The file cannot be parsed | `IO Error: Failed to open USD stage`, with USD's description |
| The file, or a file it uses (sublayer, reference, payload, value clip), is outside `allowed_directories` while `enable_external_access = false` | `Permission Error` |
| A remote path (`s3://`, `https://`, ...) | `Not implemented Error` |
