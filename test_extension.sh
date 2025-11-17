#!/bin/bash
# Test the USD extension

set -e

echo "Testing USD Extension"
echo "====================="
echo ""

# Check if extension exists
if [ ! -f "build/release/extension/usd/usd.duckdb_extension" ]; then
    echo "ERROR: Extension not found at build/release/extension/usd/usd.duckdb_extension"
    exit 1
fi

echo "✓ Extension file exists ($(ls -lh build/release/extension/usd/usd.duckdb_extension | awk '{print $5}'))"

# Check if test USD file exists
if [ ! -f "test/data/simple_scene.usda" ]; then
    echo "ERROR: Test USD file not found at test/data/simple_scene.usda"
    exit 1
fi

echo "✓ Test USD file exists"

# Check USD symbols in extension
echo ""
echo "Checking USD symbols in extension..."
if nm build/release/extension/usd/usd.duckdb_extension | grep -q "UsdStageManager"; then
    echo "✓ UsdStageManager symbols found"
else
    echo "ERROR: UsdStageManager symbols not found"
    exit 1
fi

if nm build/release/extension/usd/usd.duckdb_extension | grep -q "UsdPrimIterator"; then
    echo "✓ UsdPrimIterator symbols found"
else
    echo "ERROR: UsdPrimIterator symbols not found"
    exit 1
fi

if nm build/release/extension/usd/usd.duckdb_extension | grep -q "UsdPrimsFunction"; then
    echo "✓ UsdPrimsFunction symbols found"
else
    echo "ERROR: UsdPrimsFunction symbols not found"
    exit 1
fi

# Check USD library dependencies
echo ""
echo "Checking USD library dependencies..."
if otool -L build/release/extension/usd/usd.duckdb_extension | grep -q "libusd_usd.dylib"; then
    echo "✓ Linked against libusd_usd.dylib"
else
    echo "ERROR: Not linked against libusd_usd.dylib"
    exit 1
fi

if otool -L build/release/extension/usd/usd.duckdb_extension | grep -q "libusd_usdGeom.dylib"; then
    echo "✓ Linked against libusd_usdGeom.dylib"
else
    echo "ERROR: Not linked against libusd_usdGeom.dylib"
    exit 1
fi

if otool -L build/release/extension/usd/usd.duckdb_extension | grep -q "libusd_sdf.dylib"; then
    echo "✓ Linked against libusd_sdf.dylib"
else
    echo "ERROR: Not linked against libusd_sdf.dylib"
    exit 1
fi

echo ""
echo "====================="
echo "All checks passed! ✓"
echo "====================="
echo ""
echo "Extension built successfully with USD support!"
echo ""
echo "Note: To test the extension with DuckDB, you need a DuckDB binary"
echo "built from the same commit (c72d573027) as the extension."
echo ""

