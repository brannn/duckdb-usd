#!/usr/bin/env python3
"""Test the USD extension"""

import sys
sys.path.insert(0, '/opt/homebrew/lib/python3.13/site-packages')

import duckdb as db

# Connect to DuckDB
conn = db.connect()

# Load the extension
print("Loading USD extension...")
conn.execute("LOAD './build/release/extension/usd/usd.duckdb_extension'")
print("Extension loaded successfully!")

# Test usd_test() function
print("\nTesting usd_test() function:")
result = conn.execute('SELECT * FROM usd_test()').fetchall()
print(result)

# Test usd_prims() function with test file
print("\nTesting usd_prims() function:")
result = conn.execute("SELECT * FROM usd_prims('test/data/simple_scene.usda')").fetchall()
for row in result:
    print(f"  {row[0]}: {row[1]}")

print("\nAll tests passed!")

