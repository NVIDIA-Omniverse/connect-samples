// SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//

#pragma once

#include <omni/connect/core/MaterialAlgo.h>
#include <omni/connect/core/PrimAlgo.h>

#include <omni/log/ILog.h>

#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdUtils/pipeline.h>

///////////////////////////////////////////////////////////////////////////////////////
// This file is paired with the HelloWorld Connect Sample to demonstrate the
// USD and MDL material creation API.
///////////////////////////////////////////////////////////////////////////////////////

PXR_NAMESPACE_USING_DIRECTIVE

// createAndBindMaterial
// This function will create and OmniPBR-based material with Preview Surface shaders and bind it to the prim
//
// param: prim The prim to which the material is bound
// param: materialName The name of the material prim
// param: diffuseTexturePath The optional path to the diffuse texture, nullptr if none required
// param: normalTexturePath The optional path to the normal texture, nullptr if none required
// param: ormTexturePath The optional path to the ORM texture, nullptr if none required (not used for Preview Surface)
// param: color The diffuse color of the Material
// param: opacity The Opacity Amount to set. When less than 1.0, Enable Opacity is set to true and Fractional Opacity is enabled in the RT renderer
// param: roughness The Roughness Amount to set, 0.0-1.0 range where 1.0 = flat and 0.0 = glossy
// param: metallic The Metallic Amount to set, 0.0-1.0 range where 1.0 = max metallic and 0.0 = no metallic
// returns The newly defined UsdShadeMaterial. Returns an Invalid prim on error
static UsdShadeMaterial createAndBindMaterial(
    UsdPrim prim,
    const std::string& materialName,
    const GfVec3f& color = GfVec3f(0.463f, 0.725f, 0.0f),
    const char* diffuseTexturePath = nullptr,
    const char* normalTexturePath = nullptr,
    const char* ormTexturePath = nullptr,
    const float opacity = 1.0f,
    const float roughness = 0.5f,
    const float metallic = 0.0f
)
{
    const SdfPath& defaultPrimPath = prim.GetStage()->GetDefaultPrim().GetPath();

    // Make path for "/Looks" scope under the default prim
    const SdfPath matScopePath = defaultPrimPath.AppendChild(UsdUtilsGetMaterialsScopeName());
    UsdPrim scopePrim = UsdGeomScope::Define(prim.GetStage(), matScopePath).GetPrim();

    // Get a unique and valid material name
    TfTokenVector materialNameTokens = omni::connect::core::getValidChildNames(scopePrim, { materialName });

    // Use the Omniverse Connect SDK to define a material prim
    UsdShadeMaterial matPrim = omni::connect::core::defineOmniPbrMaterial(
        scopePrim, /* parent prim */
        materialNameTokens[0].GetString(), /* material prim name */
        color, /* material diffuse color */
        opacity,
        roughness,
        metallic
    );

    // Setup the OmniPBR MDL texture map parameters (diffuse, orm, and normal)
    // Color Map
    if (diffuseTexturePath)
    {
        omni::connect::core::addDiffuseTextureToPbrMaterial(matPrim, SdfAssetPath(diffuseTexturePath));
    }

    // ORM Map
    if (ormTexturePath)
    {
        omni::connect::core::addOrmTextureToPbrMaterial(matPrim, SdfAssetPath(ormTexturePath));
    }

    // Normal Map
    if (normalTexturePath)
    {
        omni::connect::core::addNormalTextureToPbrMaterial(matPrim, SdfAssetPath(normalTexturePath));
    }

    omni::connect::core::bindMaterial(prim, matPrim);
    return matPrim;
}
