# Contributing

## Setup

```bash
git clone --recurse-submodules git@github.com:brannn/duckdb-usd.git
cd duckdb-usd
make setup-vcpkg                                   # clones vcpkg into ./vcpkg
export VCPKG_TOOLCHAIN_PATH=$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake
export GEN=ninja
# Apple Silicon: release-only dependencies, as in CI
export VCPKG_TARGET_TRIPLET=arm64-osx-release VCPKG_HOST_TRIPLET=arm64-osx-release

make release        # DuckDB, the extension, the shell and the test runner
make test           # all tests in test/sql
```

The first build compiles OpenUSD (a few minutes) and DuckDB (longer). Later builds are incremental, and vcpkg caches
OpenUSD in `~/.cache/vcpkg/archives`.

The submodules pin DuckDB to its 2.0.0 release (`v2.0.0`) and extension-ci-tools to DuckDB's 2.0 release branch
(`v2.0-cyanoptera`).

Useful targets: `make debug`, `make format-fix` (DuckDB's formatter; run before committing),
`make clangd` (a `compile_commands.json` for editors).

### Formatting

CI checks formatting with DuckDB's formatter, which needs clang-format **11** (any 11.x), `black` and
`cmake-format`. The style comes from `.clang-format`, a link to DuckDB's. On Linux,
`pip install clang_format==11.0.1 black cmake-format` is enough. The PyPI clang-format 11 packages for macOS are
Intel-only and can hang under Rosetta, so on Apple Silicon use the native build from conda-forge:

```bash
micromamba create -p ./.fmtenv -c conda-forge clang-tools=11.0.1   # or conda / mamba
pip install black cmake-format
PATH="$PWD/.fmtenv/bin:$PATH" make format-fix
```

## Repository layout

```
src/
  usd_extension.cpp          entry point: registers functions, the replacement scan and OpenUSD setup
  usd_plugins.cpp            registers the OpenUSD plugins linked into the extension (see below)
  usd_file_access.cpp        the asset resolver that applies DuckDB's file access settings to the files USD reads
  usd_stage.cpp              opening stages: path checks, USD diagnostics, the stage cache
  usd_scan.cpp               the scan framework shared by every table function
  usd_json.cpp               USD values (VtValue) to JSON, cast to VARIANT
  functions/usd_*.cpp        one file per SQL function
  include/                   headers
cmake/UsdEmbedResources.cmake  compiles OpenUSD's plugInfo.json / generatedSchema.usda into the extension
vcpkg_ports/usd/             overlay vcpkg port: OpenUSD as static libraries
test/sql/                    sqllogictests
test/data/                   small synthetic USD scenes used by the tests
scripts/                     benchmark, test data generator, leak check
docs/                        function reference and cookbook
```

## How a scan works

Every table function is a `UsdTableFunctionDefinition` (`src/include/usd_scan.hpp`): a name, its columns, its
function-specific options and a factory for a `UsdRowProducer`. `UsdCreateScanFunction` turns a definition into a
DuckDB table function that:

1. **binds**: expands paths and globs with DuckDB's multi-file reader, checks each file, parses the options;
2. **schedules**: opens the files one at a time and hands out *morsels* of up to 1024 prims from a `UsdPrimRange`
   (or a whole stage, for stage-level functions) to DuckDB's threads, each tagged with a batch index so that
   results keep the traversal order;
3. **produces**: each thread's producer turns the prims of its morsel into buffered rows (`AddPrim`, or
   `BeginStage` for stage-level functions), and writes only the columns the query projects (`Write`).

Producers keep rows as lightweight handles (`UsdPrim`, `UsdProperty`, ...) and compute column values in `Write`, so
an unprojected column costs little. (A computation that most columns share, such as the world transform of
`usd_xforms` or the box of `usd_bounds`, is done once per row in `AddPrim`.) Write column values with the helpers at
the end of `usd_scan.hpp`: they also replace invalid UTF-8, which USD strings may contain.

Functions whose rows depend on time set `time_dependent` in their definition, which gives them the `time` and
`times` options. With `times`, the scan hands each morsel's prims to the producer once per time, calling `SetTime`
before each pass so that the producer's caches (`UsdGeomXformCache`, `UsdGeomBBoxCache`) serve the whole morsel at
one time, and writes the `time` column itself. A producer that computes a column in `Write` from a time must keep
each row's time (see `XformRow::time` in `usd_xforms.cpp`).

### Adding a function

1. Create `src/functions/usd_<name>.cpp` with a producer (usually a `UsdBufferedProducer`, which keeps the rows in
   a vector) and a `Usd<Name>Function()` that fills in a `UsdTableFunctionDefinition`, following an existing function
   (e.g. `usd_variants.cpp`). The definition's `description` and `example` are what `duckdb_functions()` shows. The
   columns are listed twice, in the function's column enum and in the definition's `columns`: keep both in the same
   order.
2. Declare `CreateTableFunctionInfo Usd<Name>Function()` in `usd_functions.hpp`, register it in `usd_extension.cpp`
   and add the file to `CMakeLists.txt`.
3. Add tests in `test/sql/` and document it in `docs/functions.md`.

### Opening stages

`UsdStageProvider::Open` (`src/usd_stage.cpp`) shares an opened stage between the scans of a query and, through
DuckDB's object cache, between queries (the `usd_stage_cache` setting). A cached stage is reused while every file it
read is unchanged (same modification time and size) and the assets it failed to load still fail. Cached stages are
budgeted (at most half of `memory_limit`, least recently used dropped first) so that caching one never pushes DuckDB
over its memory limit: when many threads evict from the object cache at once, a query can fail with a spurious
out-of-memory error.

One USD behavior shapes this. USD keeps a single in-memory copy of each layer, shared by every stage that uses the
file, and opening a stage reuses that copy even if the file has changed since. While a cached stage holds a layer,
a newly opened stage would get its old contents. So the extension records each layer file's stamp when the layer is
first loaded and checks the layers of every stage it opens; when one is out of date, it releases the cached stages
that hold it and opens the stage again.

### File access

USD reads sublayers, references, payloads and value clips itself, outside DuckDB's file system. To apply DuckDB's
file access settings (`enable_external_access`, `allowed_directories`) to those reads, the extension installs its
own asset resolver (`src/usd_file_access.cpp`): OpenUSD's default resolver, plus a check. Each stage is opened with a
resolver context that carries the settings of its database, and the resolver refuses every file they do not allow
before USD looks it up or opens it, on whichever thread USD reads it. A refused file looks missing to USD; the
extension then fails the query with a permission error, before it reports any of USD's errors, which can quote the
files they are about. USD also shares loaded layers between stages (including stages of other databases), so the
layers of every stage are checked again whenever it is used.

## OpenUSD inside the extension

A DuckDB extension ships as one file, so OpenUSD is linked into it statically. vcpkg's `usd` port only builds shared
libraries, so `vcpkg_ports/usd` overlays it:

- static libraries, limited to what the extension uses: the core libraries and OpenUSD's schema libraries, without
  the renderer-specific `usdRi` and `usdHydra`, `usdUtils`, imaging, tools or file format plugins (patch 102);
- a private C++ namespace and plugin path environment variable (`DUCKDB_USD_PLUGINPATH`), and private
  constructor sections on macOS (patch 101), so the extension can share a process with another OpenUSD (e.g. the
  Python `pxr` module);
- patch 100 keeps whole-archive flags out of the exported CMake targets; `CMakeLists.txt` links every OpenUSD
  library as a whole archive itself, because OpenUSD registers types, schemas and file formats from static
  initializers.

OpenUSD discovers file formats, the asset resolver and schemas through `plugInfo.json` files and reads each schema
library's `generatedSchema.usda`. `cmake/UsdEmbedResources.cmake` compiles those files into the extension;
`UsdRegisterPlugins()` writes them, with the `plugInfo.json` of the extension's asset resolver, to a private
temporary directory when the extension loads and registers them before any other OpenUSD call. Hosts without a
writable temporary directory can set `DUCKDB_USD_RESOURCE_DIR`.

Two rules this setup depends on:

- **One copy of TBB.** The extension's OpenUSD and TBB must be linked together into the extension. (With vcpkg's
  shared OpenUSD and a static TBB, every OpenUSD library gets a private TBB, and `UsdGeomBBoxCache` deadlocks.)
- **Matching symbol visibility.** The extension's sources are compiled with default visibility, like OpenUSD.
  DuckDB's default for extensions (hidden) duplicates the template statics OpenUSD uses to identify the types held
  by a `VtValue`, which breaks the asset resolver. Exports are still limited to the entry point (an exported symbols
  list on macOS, a version script on Linux).

To add an OpenUSD schema library (e.g. `usdRi`), keep it in patch 102 and add it to `USD_SCHEMA_LIBRARIES` in
`CMakeLists.txt`: its resources are embedded, and its plugin is checked at load, from that list.

## Tests

Tests are DuckDB sqllogictests in `test/sql/` (see DuckDB's
[testing documentation](https://duckdb.org/docs/stable/dev/sqllogictest/intro)). Run them with `make test`, or a
single file with `build/release/test/unittest test/sql/usd_prims.test`.

The scenes in `test/data/` are small hand-written `.usda` files, each built to exercise specific USD features
(inactive, abstract and undefined prims, instancing, payloads, sublayers, variants, broken references, rotations,
time samples and splines, value types, purposes, the schema domains beyond geometry). `grid.usda` is generated by
`scripts/generate_grid_scene.py` and is large enough to span several scan morsels and output chunks.

Debug builds (`make debug`, then `make test_debug`) run with AddressSanitizer. OpenUSD itself is not instrumented,
which makes ASan's container-overflow check report false positives inside OpenUSD; run debug tests with
`ASAN_OPTIONS=detect_container_overflow=0`.

When adding a test, compute the expected result independently of the extension — by hand, or with OpenUSD's Python
API (`pip install usd-core`) — rather than copying the extension's output.

## Real scenes and `data/`

`data/` is for real, large USD scenes used to check the extension at scale. **It is never committed** (it is in
`.gitignore`), and **no name, path or other identifying detail from those scenes may appear anywhere in the
repository** — not in code, tests, documentation, commit messages or issues. Write tests against synthetic scenes
in `test/data/`, with generic names.

- `scripts/benchmark.sh <file.usd> ...` times every function on the given files and prints only row counts and
  timings (files are numbered, not named).
- `scripts/check_data_leaks.sh` reads the names in every scene under `data/` (with the extension) and searches the
  repository for them. Run it before committing.

## Continuous integration

`.github/workflows/MainDistributionPipeline.yml` builds and tests the extension on DuckDB's extension CI for Linux
(x86-64 and arm64) and macOS (arm64). Windows is not a target; WebAssembly and musl Linux are excluded, as OpenUSD does
not build there.

Every CI build compiles OpenUSD and DuckDB from source, so CI runs sparingly:

- **Pull requests** build and test Linux x86-64 only. A new push to a pull request cancels its previous run.
- **Pushes to `main`** (merges) build every platform. macOS starts only once Linux has passed. Build caches saved on
  `main` speed up every pull request.
- **Manual runs** (*Actions → Main Extension Distribution Pipeline → Run workflow*) build every platform. Use one to
  check macOS or Linux arm64 before merging.
- **Documentation-only changes** (`*.md`, `docs/`, `LICENSE`) do not run CI.
- **Pushes to other branches** do not run CI; open a pull request.
- **Formatting** is checked first, without a build. The build starts only once the format check passes, so run
  `make format-fix` before pushing.
