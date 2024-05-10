# SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: MIT
#

# Omni imports
import omni.connect.core
from pxr import Gf, UsdGeom, UsdUtils


def createAndBindMaterial(
    prim,
    materialName,
    color=Gf.Vec3f(0.463, 0.725, 0.0),
    diffuseTexturePath=None,
    normalTexturePath=None,
    ormTexturePath=None,
    opacity=1.0,
    roughness=0.5,
    metallic=0.0,
):
    """
    This function will create and OmniPBR-based material with Preview Surface shaders and bind it to the prim

    param: prim The prim to which the material is bound
    param: materialName The name of the material prim
    param: diffuseTexturePath The optional path to the diffuse texture, nullptr if none required
    param: normalTexturePath The optional path to the normal texture, nullptr if none required
    param: ormTexturePath The optional path to the ORM texture, nullptr if none required (not used for Preview Surface)
    param: color The diffuse color of the Material
    param: opacity The Opacity Amount to set. When less than 1.0, Enable Opacity is set to true and Fractional Opacity is enabled in the RT renderer
    param: roughness The Roughness Amount to set, 0.0-1.0 range where 1.0 = flat and 0.0 = glossy
    param: metallic The Metallic Amount to set, 0.0-1.0 range where 1.0 = max metallic and 0.0 = no metallic
    returns: The newly defined UsdShadeMaterial. Returns an Invalid prim on error
    """
    # Create a Materials scope
    default_prim_path = prim.GetStage().GetDefaultPrim().GetPath()
    scope_prim = UsdGeom.Scope.Define(prim.GetStage(), default_prim_path.AppendPath(UsdUtils.GetMaterialsScopeName()))

    # Get a unique and valid material name
    material_name_tokens = omni.connect.core.getValidChildNames(
        scope_prim.GetPrim(),
        [
            materialName,
        ],
    )

    # Create a material instance for this in USD
    matPrim = omni.connect.core.defineOmniPbrMaterial(scope_prim.GetPrim(), material_name_tokens[0], color, opacity, roughness, metallic)

    if diffuseTexturePath:
        omni.connect.core.addDiffuseTextureToPbrMaterial(matPrim, diffuseTexturePath)
    if ormTexturePath:
        omni.connect.core.addOrmTextureToPbrMaterial(matPrim, ormTexturePath)
    if normalTexturePath:
        omni.connect.core.addNormalTextureToPbrMaterial(matPrim, normalTexturePath)

    omni.connect.core.bindMaterial(prim, matPrim)
    return matPrim
