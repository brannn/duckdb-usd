#!/bin/bash
# Benchmark USD extension with large GRR02 sector file

set -e

export PATH=~/brew/bin:$PATH

EXTENSION_PATH="./build/release/extension/usd/usd.duckdb_extension"
USD_FILE="./assets/grr02_sec01/_GRR02_SEC_1_TSC_150_GB300_fully_fixed.usd"

echo "=========================================="
echo "USD Extension Performance Benchmark"
echo "=========================================="
echo "Extension: $EXTENSION_PATH"
echo "USD File: $USD_FILE"
echo "File Size: $(ls -lh "$USD_FILE" | awk '{print $5}')"
echo ""

# Test 1: Count all prims
echo "Test 1: Count all prims"
echo "----------------------------------------"
time duckdb -unsigned -c "
LOAD '$EXTENSION_PATH';
SELECT COUNT(*) as total_prims FROM usd_prims('$USD_FILE');
"
echo ""

# Test 2: Group by prim_type
echo "Test 2: Group by prim_type (top 10)"
echo "----------------------------------------"
time duckdb -unsigned -c "
LOAD '$EXTENSION_PATH';
SELECT prim_type, COUNT(*) as count
FROM usd_prims('$USD_FILE')
GROUP BY prim_type
ORDER BY count DESC
LIMIT 10;
"
echo ""

# Test 3: Filter by kind
echo "Test 3: Count by kind"
echo "----------------------------------------"
time duckdb -unsigned -c "
LOAD '$EXTENSION_PATH';
SELECT kind, COUNT(*) as count
FROM usd_prims('$USD_FILE')
WHERE kind != ''
GROUP BY kind
ORDER BY count DESC;
"
echo ""

# Test 4: Complex query with multiple filters
echo "Test 4: Complex query - components with specific type"
echo "----------------------------------------"
time duckdb -unsigned -c "
LOAD '$EXTENSION_PATH';
SELECT COUNT(*) as component_xforms
FROM usd_prims('$USD_FILE')
WHERE kind = 'component' AND prim_type = 'Xform';
"
echo ""

# Test 5: Hierarchy analysis
echo "Test 5: Hierarchy depth analysis"
echo "----------------------------------------"
time duckdb -unsigned -c "
LOAD '$EXTENSION_PATH';
SELECT 
    LENGTH(prim_path) - LENGTH(REPLACE(prim_path, '/', '')) as depth,
    COUNT(*) as count
FROM usd_prims('$USD_FILE')
GROUP BY depth
ORDER BY depth;
"
echo ""

# Test 6: Active/Instanceable stats
echo "Test 6: Active and Instanceable statistics"
echo "----------------------------------------"
time duckdb -unsigned -c "
LOAD '$EXTENSION_PATH';
SELECT 
    active,
    instanceable,
    COUNT(*) as count
FROM usd_prims('$USD_FILE')
GROUP BY active, instanceable;
"
echo ""

echo "=========================================="
echo "Benchmark Complete!"
echo "=========================================="

