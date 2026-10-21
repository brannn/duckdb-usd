#!/usr/bin/env bash
# Times the usd_* table functions on one or more USD files and prints row counts and timings only.
#
# Usage: scripts/benchmark.sh <file.usd> [<file.usd> ...]
#
# Uses the DuckDB shell and extension from a release build (`make release`). Override with DUCKDB=... and
# USD_EXTENSION=... to benchmark other builds.
set -euo pipefail

if [ "$#" -eq 0 ]; then
	echo "usage: $0 <file.usd> [<file.usd> ...]" >&2
	exit 1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DUCKDB="${DUCKDB:-$ROOT/build/release/duckdb}"
USD_EXTENSION="${USD_EXTENSION:-$ROOT/build/release/extension/usd/usd.duckdb_extension}"

now() {
	perl -MTime::HiRes=time -e 'printf "%.3f", time'
}

run() {
	local label="$1" sql="$2"
	local start end result
	start=$(now)
	# strip the color codes of warnings the shell prints (e.g. about composition errors)
	result=$("$DUCKDB" -no-agent -unsigned -noheader -csv -c "LOAD '$USD_EXTENSION'; SET enable_progress_bar = false; $sql" 2>/dev/null |
		perl -pe 's/\e\[[0-9;]*m//g' | tail -1)
	end=$(now)
	printf "  %-44s %14s rows  %7.2fs\n" "$label" "$result" "$(echo "$end - $start" | bc)"
}

index=0
for file in "$@"; do
	index=$((index + 1))
	f="${file//\'/\'\'}"
	# files are numbered rather than named, so that the output can be shared without revealing asset names
	echo "file $index of $# ($(du -h "$file" | cut -f1) root layer)"
	run "usd_layers" "SELECT count(*) FROM usd_layers('$f')"
	run "usd_composition_errors" "SELECT count(*) FROM usd_composition_errors('$f')"
	run "usd_prims" "SELECT count(*) FROM usd_prims('$f')"
	run "usd_prims (all columns)" "SELECT count(*) FROM (SELECT * FROM usd_prims('$f'))"
	run "usd_prims (instance_proxies)" "SELECT count(*) FROM usd_prims('$f', instance_proxies := true)"
	run "usd_properties" "SELECT count(*) FROM usd_properties('$f')"
	run "usd_properties (values)" "SELECT count(value) FROM usd_properties('$f')"
	run "usd_properties (authored values)" "SELECT count(value) FROM usd_properties('$f', authored_only := true)"
	run "usd_relationships" "SELECT count(*) FROM usd_relationships('$f')"
	run "usd_xforms" "SELECT count(*) FROM (SELECT * FROM usd_xforms('$f'))"
	run "usd_bounds" "SELECT count(min_x) FROM usd_bounds('$f')"
	run "usd_variants" "SELECT count(*) FROM usd_variants('$f')"
	run "usd_composition_arcs" "SELECT count(*) FROM usd_composition_arcs('$f')"
	run "usd_material_bindings" "SELECT count(*) FROM usd_material_bindings('$f')"
	run "usd_time_samples (values)" "SELECT count(value) FROM usd_time_samples('$f')"
	run "usd_xforms (10 times)" "SELECT count(*) FROM (SELECT * FROM usd_xforms('$f', times := range(0, 10)))"
done
