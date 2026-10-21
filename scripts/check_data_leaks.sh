#!/usr/bin/env bash
# Checks that no prim, property or file name from the local USD scenes in data/ appears in the repository.
#
# data/ holds real scenes that must never be tracked, and nothing identifying from them may appear in code, tests
# or documentation. This script reads the names from the scenes at run time (with the extension itself), keeps the
# distinctive ones, and searches every file git would track for them. It prints the matching files and lines (with
# the names masked), and exits 1 if there are any. The names themselves are only held in a temporary file that is
# deleted on exit.
#
# With --history, it searches every file version and commit message reachable from any ref instead (run it before
# pushing).
#
# Usage: scripts/check_data_leaks.sh [--history]            (requires `make release`)
set -euo pipefail

case "${1:-}" in
"" | --history) ;;
*)
	echo "usage: $0 [--history]" >&2
	exit 2
	;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DUCKDB="${DUCKDB:-$ROOT/build/release/duckdb}"
USD_EXTENSION="${USD_EXTENSION:-$ROOT/build/release/extension/usd/usd.duckdb_extension}"
cd "$ROOT"

if [ ! -d data ]; then
	echo "no data/ directory - nothing to check"
	exit 0
fi

TOKENS="$(mktemp)"
CANDIDATES="$(mktemp)"
VOCABULARY="$(mktemp)"
trap 'rm -f "$TOKENS" "$CANDIDATES" "$VOCABULARY"' EXIT

# Ordinary words are not identifying: English words (the system dictionary, if there is one, with the plurals it
# leaves out) and the vocabulary of DuckDB's own sources, which covers the programming terms the repository uses
{
	if [ -f /usr/share/dict/words ]; then
		awk '{ print; print $0 "s"; print $0 "es" } /y$/ { print substr($0, 1, length($0) - 1) "ies" }' \
			/usr/share/dict/words
	fi
	grep -rhoE '[A-Za-z][A-Za-z0-9]{4,}' duckdb/src/include 2>/dev/null || true
} | tr '[:upper:]' '[:lower:]' | sort -u > "$VOCABULARY"

# every USD layer under data/, as a SQL list literal
FILE_LIST="$(find data -type f \( -name '*.usd' -o -name '*.usda' -o -name '*.usdc' \) -print |
	sed "s/'/''/g; s/^/'/; s/$/'/" | paste -sd, -)"
if [ -z "$FILE_LIST" ]; then
	echo "no USD files in data/ - nothing to check"
	exit 0
fi

# Names of prims, properties and files in every scene, split into tokens. Short tokens and the vocabulary of USD
# itself (schema, property and metadata names) are not distinctive, so they are dropped.
"$DUCKDB" -no-agent -unsigned -noheader -list -c "
LOAD '$USD_EXTENSION';
SET enable_progress_bar = false;
WITH files AS (
    SELECT unnest([$FILE_LIST]) AS file
),
prims AS (
    SELECT name, prim_type
    FROM usd_prims([$FILE_LIST], include_abstract := true, include_undefined := true, ignore_errors := true)
),
properties AS (
    SELECT property_name, type_name, is_custom
    FROM usd_properties([$FILE_LIST], authored_only := true, ignore_errors := true)
),
names AS (
    SELECT name FROM prims
    UNION ALL
    SELECT property_name FROM properties WHERE is_custom
    UNION ALL
    SELECT unnest(string_split(file, '/')) FROM files
),
tokens AS (
    SELECT DISTINCT lower(token) AS token
    FROM names, unnest(regexp_split_to_array(name, '[^A-Za-z0-9]+')) AS t(token)
    WHERE length(token) >= 5 AND NOT regexp_full_match(token, '[0-9]+')
),
-- the words of USD itself: schema property names, value types and prim types
usd_vocabulary AS (
    SELECT DISTINCT lower(token) AS token
    FROM (SELECT property_name || ' ' || coalesce(type_name, '') AS words FROM properties WHERE NOT is_custom
          UNION ALL
          SELECT prim_type FROM prims WHERE prim_type IS NOT NULL),
         unnest(regexp_split_to_array(words, '[^A-Za-z0-9]+')) AS t(token)
)
SELECT token FROM tokens
WHERE token NOT IN (SELECT token FROM usd_vocabulary)
  AND token NOT IN ('scene', 'scenes', 'world', 'group', 'model', 'models', 'layer', 'layers', 'material',
                    'materials', 'shader', 'shaders', 'looks', 'light', 'lights', 'camera', 'geometry', 'object',
                    'objects', 'collected', 'textures', 'texture', 'assets', 'asset', 'files', 'stage', 'xform',
                    'scope', 'render', 'proxy', 'guide', 'default', 'surface', 'preview', 'cylinder', 'sphere',
                    'other', 'parts', 'mesh', 'meshes', 'instance', 'instances', 'prototype', 'prototypes')
ORDER BY token;
" > "$CANDIDATES"
grep -v -x -F -f "$VOCABULARY" "$CANDIDATES" > "$TOKENS" || true

# Prints the lines of stdin that contain a name, prefixed with $1 and the line number. Names are matched as whole
# words, with underscores as separators (grep -w would treat "Name_01" as one word). The names themselves are masked
# as <name>, so that the output never repeats them.
match_names() {
	LC_ALL=C sed 's/_/ /g' | LC_ALL=C grep -I -n -i -w -F -f "$TOKENS" | mask_names | sed "s|^|$1:|" || true
}

mask_names() {
	perl -ne '
		BEGIN { open(my $f, "<", shift) or die; @names = grep { length } map { chomp; quotemeta } <$f> }
		$re //= join("|", sort { length($b) <=> length($a) } @names);
		s/(?<![A-Za-z0-9])(?:$re)(?![A-Za-z0-9])/<name>/gi;
		print;
	' "$TOKENS"
}

MATCHES=""
if [ "${1:-}" = --history ]; then
	echo "checking $(wc -l < "$TOKENS" | tr -d ' ') distinctive names from data/ against the git history"
	# every file version reachable from any ref (named by the first path it appears at) and every commit message
	while read -r type object path; do
		if [ "$type" = blob ]; then
			MATCHES+="$(git cat-file blob "$object" | match_names "$path@${object:0:10}")"$'\n'
		fi
	done < <(git rev-list --objects --all | git cat-file --batch-check='%(objecttype) %(objectname) %(rest)')
	for commit in $(git rev-list --all); do
		MATCHES+="$(git log -1 --format=%B "$commit" | match_names "commit ${commit:0:10} message")"$'\n'
	done
else
	echo "checking $(wc -l < "$TOKENS" | tr -d ' ') distinctive names from data/ against the repository"
	# every file git would track: tracked files plus untracked files that are not ignored
	while read -r file; do
		if [ -f "$file" ]; then
			MATCHES+="$(match_names "$file" < "$file")"$'\n'
		fi
	done < <(git ls-files --cached --others --exclude-standard | grep -v '^data/' || true)
fi
MATCHES="$(echo "$MATCHES" | sed '/^$/d')"
if [ -n "$MATCHES" ]; then
	echo "Possible leaks of names from data/ (review each one):"
	echo "$MATCHES"
	exit 1
fi
echo "no names from data/ found"
