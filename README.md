# DuckDB USD Extension

Query [Universal Scene Description](https://openusd.org) (USD) files with SQL.

The `usd` extension for [DuckDB](https://duckdb.org) turns the contents of a USD stage — prims, properties,
relationships, transforms, bounding boxes, variants, layers, composition arcs, material bindings, time samples and
composition errors — into tables. It is built for large scenes with millions of prims spread over hundreds of
layers: digital twins of buildings, factories and infrastructure, simulation environments, and film and game assets.
Inventories, audits and spatial questions become SQL queries, with no USD traversal code to write and no graphical
client to open.

```sql
LOAD usd;

-- what is in the scene?
SELECT prim_type, count(*) FROM usd_prims('facility.usd') GROUP BY ALL ORDER BY 2 DESC;

-- where is it?
SELECT prim_path, x, y, z FROM usd_xforms('facility.usd') WHERE prim_path LIKE '/Site/Zone_A/%';

-- what does it say about itself?
SELECT prim_path, property_name, value FROM usd_properties('facility.usd', authored_only := true) WHERE is_custom;

-- is anything broken?
SELECT error_type, prim_path, message FROM usd_composition_errors('facility.usd');
```

The extension targets **DuckDB 2.0** and links **OpenUSD 26.05** statically, so it is a single self-contained file.
It runs on Linux (x86-64 and arm64) and macOS (Apple Silicon).

## Functions

| Function | One row per | Highlights |
|---|---|---|
| [`usd_prims(path)`](docs/functions.md#usd_prims) | prim | type, kind, specifier, active, instancing, payloads, applied schemas, `custom_data`, `asset_info` |
| [`usd_properties(path)`](docs/functions.md#usd_properties) | attribute or relationship | type, authored/custom flags, time samples, typed `value` (`VARIANT`) |
| [`usd_relationships(path)`](docs/functions.md#usd_relationships) | relationship target | target paths (optionally forwarded) |
| [`usd_xforms(path)`](docs/functions.md#usd_xforms) | xformable prim | world position, rotation, scale, world and local matrices |
| [`usd_bounds(path)`](docs/functions.md#usd_bounds) | imageable prim | world-space axis-aligned bounding box |
| [`usd_variants(path)`](docs/functions.md#usd_variants) | variant set | selection and available variants |
| [`usd_layers(path)`](docs/functions.md#usd_layers) | layer used by the stage | identifier, resolved path, format, sublayers |
| [`usd_stage_metadata(path)`](docs/functions.md#usd_stage_metadata) | stage | up axis, meters per unit, time codes, custom layer data |
| [`usd_composition_errors(path)`](docs/functions.md#usd_composition_errors) | composition error | broken references, invalid paths, cycles |
| [`usd_composition_arcs(path)`](docs/functions.md#usd_composition_arcs) | composition arc of a prim | references, payloads, inherits, specializes and variant selections: which assets are used where |
| [`usd_material_bindings(path)`](docs/functions.md#usd_material_bindings) | prim bound to a material | the resolved material, and the direct or collection binding (own or inherited) that decides it |
| [`usd_time_samples(path)`](docs/functions.md#usd_time_samples) | time sample of an attribute | what is recorded over time (animation, sensor readings, simulation results), including spline knots and value clips |
| [`usd_version()`](docs/functions.md#usd_version) | — | the OpenUSD version the extension was built with |

`SELECT * FROM 'scene.usda'` is shorthand for `usd_prims('scene.usda')`.

Every table function takes a file path, a glob (`'scenes/*.usd'`) or a list of paths, plus these named options:

| Option | Default | Meaning |
|---|---|---|
| `root` | `'/'` | Scan (and compose) only this prim's subtree, e.g. `root := '/Site/Zone_A'` |
| `instance_proxies` | `false` | Also visit the prims inside instances |
| `active_only` | `false` | Skip inactive prims (by default they are listed, but their children are not composed) |
| `include_abstract` | `false` | Also visit abstract prims (`class` specifiers) |
| `include_undefined` | `false` | Also visit prims that only have `over` opinions |
| `load_payloads` | `true` | Load payloads (`false` composes the stage without them) |
| `filename` | `false` | Add a `filename` column (useful with globs) |
| `ignore_errors` | `false` | Skip files that cannot be read (missing, not USD, failing to open) instead of failing |

`usd_properties`, `usd_xforms` and `usd_bounds` read at the default time, at a time code (`time := 24`), or at
several (`times := range(0, 241, 24)`: one row per time, with a `time` column), so moving things can be followed
through time.

See [docs/functions.md](docs/functions.md) for every column and function-specific option, and
[docs/cookbook.md](docs/cookbook.md) for recipes, including [maps and GIS](docs/cookbook.md#maps-and-gis) with
DuckDB's spatial extension.

## Installation

The extension is published in DuckDB's [community extensions](https://duckdb.org/community_extensions/):

```sql
INSTALL usd FROM community;
LOAD usd;
```

A build from source (below) loads with `LOAD 'build/release/extension/usd/usd.duckdb_extension';`. DuckDB only
loads unsigned extensions when started with `-unsigned` (CLI) or `allow_unsigned_extensions` (clients). A build
loads only into the DuckDB version it was built against: the `build/release/duckdb` shell next to it, or a client
of the same DuckDB release.

## How it works

- **Streaming and parallel.** Prims are traversed in batches that are processed by all of DuckDB's threads. Results
  keep the traversal order.
- **Only the requested columns are computed.** `SELECT count(*) FROM usd_properties(...)` never reads a value; a
  query that selects `value` does. Array values longer than `max_array_length` (default 1000) are returned as `NULL`
  with their length in `array_length`, so a `SELECT *` over a scene full of meshes stays fast.
- **Typed values.** Property values, `custom_data`, `asset_info` and `custom_layer_data` are `VARIANT`s that keep
  their USD types: cast them (`value::DOUBLE`, `value::DOUBLE[]`) or extract fields (`custom_data.owner`).
- **Stages stay open between queries.** The first query on a large scene composes it (seconds); later queries reuse
  the composed stage until a file it read changes. Cached stages take at most half of `memory_limit` and are dropped
  when DuckDB needs the memory (`SET usd_stage_cache = false` turns this off).
- **DuckDB's file access rules apply.** With `enable_external_access = false`, only files in `allowed_directories`
  can be read — including every file a stage pulls in through sublayers, references, payloads and value clips. USD
  never opens a file outside them: a stage that needs one fails with a permission error.
- **USD diagnostics don't go to stderr.** A stage with composition errors produces a single warning in DuckDB's log
  that points to `usd_composition_errors()`; USD's own messages are logged at debug level
  (`CALL enable_logging(level := 'debug')`).

## Limitations

- Local files only: remote paths (`s3://`, `https://`) are not supported.
- Read-only.
- The extension includes OpenUSD's schemas — geometry, shading, lights, skeletons, physics, semantic labels,
  volumes, spatial audio, render settings, procedurals and UI — but not the renderer-specific `usdRi` and `usdHydra`
  or third-party schemas (e.g. a simulator's own physics schemas). Prims of those types are listed with their type
  name, but USD does not know their schema: they have no fallback properties and do not count as xformable or
  imageable.
- Bounding boxes are world-space and axis-aligned.

## Building from source

Requirements: CMake 3.26+, a C++17 compiler, Ninja (recommended), Python 3 and Git.

```bash
git clone --recurse-submodules git@github.com:brannn/duckdb-usd.git
cd duckdb-usd

# vcpkg builds OpenUSD (statically, via vcpkg_ports/usd) on the first build
make setup-vcpkg
export VCPKG_TOOLCHAIN_PATH=$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake

GEN=ninja make release
make test
```

On Apple Silicon, also set `VCPKG_TARGET_TRIPLET=arm64-osx-release VCPKG_HOST_TRIPLET=arm64-osx-release` to build
release-only dependencies. The build produces:

- `build/release/duckdb` — the DuckDB shell
- `build/release/extension/usd/usd.duckdb_extension` — the loadable extension
- `build/release/test/unittest` — the test runner

See [CONTRIBUTING.md](CONTRIBUTING.md) for the architecture, tests and benchmarks.

## License

MIT — see [LICENSE](LICENSE). The extension binary includes two third-party libraries:
[OpenUSD](https://github.com/PixarAnimationStudios/OpenUSD), under the
[Tomorrow Open Source Technology License](https://github.com/PixarAnimationStudios/OpenUSD/blob/release/LICENSE.txt),
and [oneTBB](https://github.com/uxlfoundation/oneTBB), under the
[Apache License 2.0](https://github.com/uxlfoundation/oneTBB/blob/master/LICENSE.txt). Their
license and notice texts are in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
