#!/usr/bin/env python3
"""
Create a simple test USD file for testing the DuckDB USD extension.
"""

from pxr import Usd, UsdGeom, Sdf

def create_simple_scene(output_path):
    """Create a simple USD scene with a few prims."""
    
    # Create a new stage
    stage = Usd.Stage.CreateNew(output_path)
    
    # Define the default prim
    root_prim = stage.DefinePrim("/World", "Xform")
    stage.SetDefaultPrim(root_prim)
    
    # Create a cube
    cube = UsdGeom.Cube.Define(stage, "/World/Cube")
    cube.GetSizeAttr().Set(2.0)
    
    # Create a sphere
    sphere = UsdGeom.Sphere.Define(stage, "/World/Sphere")
    sphere.GetRadiusAttr().Set(1.5)
    
    # Create a cylinder
    cylinder = UsdGeom.Cylinder.Define(stage, "/World/Cylinder")
    cylinder.GetHeightAttr().Set(3.0)
    cylinder.GetRadiusAttr().Set(0.5)
    
    # Create a nested group
    group = stage.DefinePrim("/World/Group", "Xform")
    
    # Add a mesh under the group
    mesh = UsdGeom.Mesh.Define(stage, "/World/Group/Mesh")
    
    # Save the stage
    stage.GetRootLayer().Save()
    print(f"Created test USD file: {output_path}")
    
    # Print prim hierarchy
    print("\nPrim hierarchy:")
    for prim in stage.Traverse():
        indent = "  " * (prim.GetPath().pathElementCount - 1)
        print(f"{indent}{prim.GetPath()} ({prim.GetTypeName()})")

if __name__ == "__main__":
    import sys
    import os
    
    # Default output path
    output_path = "test/data/simple_scene.usda"
    
    if len(sys.argv) > 1:
        output_path = sys.argv[1]
    
    # Ensure directory exists
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    
    create_simple_scene(output_path)

