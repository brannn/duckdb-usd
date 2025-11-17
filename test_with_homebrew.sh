#!/bin/bash
# Test USD extension using Homebrew DuckDB binary

set -e

export PATH=~/brew/bin:$PATH

EXTENSION_PATH="./build/release/extension/usd/usd.duckdb_extension"
TEST_FILE="test/data/simple_scene.usda"

echo "Testing USD Extension with Homebrew DuckDB..."
echo "Extension: $EXTENSION_PATH"
echo "Test file: $TEST_FILE"
echo ""

# Test 1: Basic query with all columns
echo "Test 1: Query all columns..."
duckdb -unsigned -c "
LOAD '$EXTENSION_PATH';
SELECT prim_path, parent_path, name, prim_type, kind, active, instanceable 
FROM usd_prims('$TEST_FILE') 
ORDER BY prim_path;
"

# Test 2: Count prims
echo ""
echo "Test 2: Count prims..."
duckdb -unsigned -c "
LOAD '$EXTENSION_PATH';
SELECT COUNT(*) as total_prims FROM usd_prims('$TEST_FILE');
"

# Test 3: Filter by parent_path
echo ""
echo "Test 3: Filter by parent_path..."
duckdb -unsigned -c "
LOAD '$EXTENSION_PATH';
SELECT name FROM usd_prims('$TEST_FILE') WHERE parent_path = '/World' ORDER BY name;
"

# Test 4: Filter by name
echo ""
echo "Test 4: Filter by name..."
duckdb -unsigned -c "
LOAD '$EXTENSION_PATH';
SELECT prim_path, prim_type FROM usd_prims('$TEST_FILE') WHERE name = 'Mesh';
"

# Test 5: Check active column
echo ""
echo "Test 5: Check active column..."
duckdb -unsigned -c "
LOAD '$EXTENSION_PATH';
SELECT COUNT(*) as active_prims FROM usd_prims('$TEST_FILE') WHERE active = true;
"

# Test 6: Check instanceable column
echo ""
echo "Test 6: Check instanceable column..."
duckdb -unsigned -c "
LOAD '$EXTENSION_PATH';
SELECT COUNT(*) as non_instanceable FROM usd_prims('$TEST_FILE') WHERE instanceable = false;
"

# Test 7: Group by prim_type
echo ""
echo "Test 7: Group by prim_type..."
duckdb -unsigned -c "
LOAD '$EXTENSION_PATH';
SELECT prim_type, COUNT(*) as count 
FROM usd_prims('$TEST_FILE') 
GROUP BY prim_type 
ORDER BY count DESC, prim_type;
"

echo ""
echo "All tests passed! ✅"

