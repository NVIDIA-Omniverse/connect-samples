// SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//

#pragma once

#include <omni/connect/core/MeshAlgo.h>
#include <omni/connect/core/PrimAlgo.h>
#include <omni/connect/core/XformAlgo.h>

#include <pxr/usd/kind/registry.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdSkel/animation.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/root.h>
#include <pxr/usd/usdSkel/skeleton.h>
#include <pxr/usd/usdSkel/utils.h>

///////////////////////////////////////////////////////////////////////////////////////
// This file is paired with the HelloWorld Connect Sample to demonstrate the
// USD Skeletal Mesh API.
///////////////////////////////////////////////////////////////////////////////////////


PXR_NAMESPACE_USING_DIRECTIVE

namespace
{
static constexpr const char g_skelRootGroupName[] = "SkelRoot_Group";
static constexpr const char g_skelName[] = "Skel";
static const double g_boneSize = 100.0;

// Get the skeleton prim from the expected path
UsdPrim getSkeletonPrim(UsdStageRefPtr stage)
{
    // We're just hardcoding the skel prim path
    const SdfPath& defaultPrimPath = stage->GetDefaultPrim().GetPath();
    std::string skelRootName(g_skelRootGroupName);
    const SdfPath skelRootPrimPath = defaultPrimPath.AppendChild(TfToken(TfMakeValidIdentifier(skelRootName)));
    std::string skelName(g_skelName);
    const SdfPath skelPrimPath = skelRootPrimPath.AppendChild(TfToken(TfMakeValidIdentifier(skelName)));

    return stage->GetPrimAtPath(skelPrimPath);
}

// Get all of the joint tokens from the skeleton
VtTokenArray getJointsFromSkeleton(UsdStageRefPtr stage)
{
    VtTokenArray jointTokens;

    UsdPrim skelPrim = ::getSkeletonPrim(stage);
    if (skelPrim)
    {
        UsdSkelSkeleton skel = UsdSkelSkeleton(skelPrim);
        if (skel)
        {
            skel.GetJointsAttr().Get(&jointTokens);
        }
    }
    return jointTokens;
}

// Bind an animation to a skeleton
// param: stage The USD stage for the skeleton and the animation
// param: animPrimPath The path to the animation prim to be created
void bindAnimToSkel(UsdStageRefPtr stage, const SdfPath& animPrimPath)
{
    UsdPrim skelPrim = ::getSkeletonPrim(stage);
    if (skelPrim)
    {
        UsdSkelBindingAPI skeletonBinding = UsdSkelBindingAPI(skelPrim);
        if (skeletonBinding)
        {
            skeletonBinding.CreateAnimationSourceRel().SetTargets(SdfPathVector({ animPrimPath }));
        }
    }
}

} // namespace


// Create an example animation for the example skeleton
//
// param: stage The USD stage for the skeleton and the animation
// param: animPrimPath The path to the animation prim to be created
// param: elbowMaxAngle The max angle for the elbow joint in the animation (on the joint's X axis)
// param: wristMaxAngle The max angle for the wrist joint in the animation (on the joint's Z axis)
void createAndBindAnimForSkel(UsdStageRefPtr stage, const SdfPath& animPrimPath, double elbowMaxAngle, double wristMaxAngle)
{
    UsdSkelAnimation anim = UsdSkelAnimation::Define(stage, animPrimPath);

    // Create the joint array (rotating the elbow and wrist)
    const VtTokenArray jointTokens = getJointsFromSkeleton(stage);
    VtTokenArray animJointTokens = {
        jointTokens[1], // elbow
        jointTokens[2] // wrist
    };
    anim.CreateJointsAttr(VtValue(animJointTokens));

    // Set constant relative translation and scale attributes
    VtVec3fArray translations = {
        GfVec3f(0, 0, g_boneSize), // elbow
        GfVec3f(0, 0, g_boneSize) // wrist
    };

    // Rotate the elbow
    std::vector<GfRotation> elbowRots = { GfRotation(GfVec3d(1, 0, 0), 0),
                                          GfRotation(GfVec3d(1, 0, 0), elbowMaxAngle),
                                          GfRotation(GfVec3d(1, 0, 0), 0) };

    // Rotate the wrist
    std::vector<GfRotation> wristRots = { GfRotation(GfVec3d(1, 0, 0), 0),
                                          GfRotation(GfVec3d(0, 0, 1), wristMaxAngle),
                                          GfRotation(GfVec3d(1, 0, 0), 0) };

    // Time samples over 2 seconds (48 frames at 24 FPS)
    std::vector<UsdTimeCode> timeCodes = { UsdTimeCode(0), UsdTimeCode(23), UsdTimeCode(47) };

    // As indicated in https://openusd.org/dev/api/_usd_skel__a_p_i__intro.html#UsdSkel_API_WritingSkels one may use
    // UsdSkelAnimation::SetTransforms() rather than setting the vectorized arrays of translation, rotation, and scale separately.
    // In a DCC app there may be a matrix for every joint every frame.  For the sake of demonstration
    // we've used the above translations, scales, and rotations
    for (size_t i = 0; i < timeCodes.size(); ++i)
    {
        VtMatrix4dArray mat4ds(2);
        UsdSkelMakeTransform<GfMatrix4d>(translations[0], elbowRots[i], GfVec3h(1), &mat4ds[0]);
        UsdSkelMakeTransform<GfMatrix4d>(translations[1], wristRots[i], GfVec3h(1), &mat4ds[1]);
        anim.SetTransforms(mat4ds, timeCodes[i]);
    }

    bindAnimToSkel(stage, animPrimPath);
}


// Set the elbow joint's relative X axis translation at time=0 for skeleton's current animation
//
// param: stage The USD stage for the skeleton and the animation
// param: elbowXTranslation The relative translation on the elbow joint's X axis from the root
void setElbowRelativeXTranslation(UsdStageRefPtr stage, double elbowXTranslation)
{
    UsdPrim skelPrim = ::getSkeletonPrim(stage);
    if (skelPrim)
    {
        UsdSkelBindingAPI skeletonBinding = UsdSkelBindingAPI(skelPrim);
        if (skeletonBinding)
        {
            UsdPrim animPrim;
            if (skeletonBinding.GetAnimationSource(&animPrim))
            {
                UsdSkelAnimation anim = UsdSkelAnimation(animPrim);
                if (anim)
                {
                    VtVec3fArray translations = {
                        GfVec3f(elbowXTranslation, 0, g_boneSize), // elbow
                        GfVec3f(-elbowXTranslation, 0, g_boneSize) // wrist (must negate since these are relative to the parent joint)
                    };
                    anim.GetTranslationsAttr().Set(translations, UsdTimeCode(0));
                }
            }
        }
    }
}


// Create a simple skinned skel mesh quad with an animation
//
// This function creates a SkelRoot as the parent prim for
// a Skeleton, Skeleton Animation, and Mesh.  The Mesh is
// skinned to the skeleton, and the skeleton sets the animation
// as its animation source.  Extents are also computed and authored
// for the various boundable (skel root, skeleton) and point-based (mesh) prims.
//
// This function will also modify the stage metadata to set the frame rate to 24
// and the end time code to 48.
//
// Learn more from the OpenUSD docs: https://openusd.org/dev/api/_usd_skel__schema_overview.html#UsdSkel_SchemaOverview_DefiningSkeletons
//
// param: stage The USD stage for the skeleton and the animation
// param: initialTranslation The initial world translation for the skeleton
static void createSkelMesh(UsdStageRefPtr stage, const GfVec3d& initialTranslation = GfVec3d(0))
{
    // A vec3f array to store the results of ComputeExtents()
    VtVec3fArray extent;

    // Create the skelroot group under the default prim
    const SdfPath& defaultPrimPath = stage->GetDefaultPrim().GetPath();
    std::string skelRootName(::g_skelRootGroupName);
    const SdfPath skelRootPrimPath = defaultPrimPath.AppendChild(TfToken(TfMakeValidIdentifier(skelRootName)));

    //////////////
    // SkelRoot //
    //////////////
    UsdSkelRoot skelRoot = UsdSkelRoot::Define(stage, skelRootPrimPath);
    UsdSkelBindingAPI::Apply(skelRoot.GetPrim());

    // A UsdSkel should be moved around at or above its SkelRoot, push it away from the center of the stage
    omni::connect::core::setLocalTransform(
        skelRoot.GetPrim(),
        initialTranslation, // translation
        GfVec3d(0), // pivot
        GfVec3f(0), // rotation
        omni::connect::core::RotationOrder::eXyz,
        GfVec3f(1) // scale
    );

    // Make the kind a component to support the assembly/component selection hierarchy
    pxr::UsdModelAPI(skelRoot.GetPrim()).SetKind(pxr::KindTokens->component);


    //////////////
    // Skeleton //
    //////////////
    std::string skelName(::g_skelName);
    const SdfPath skelPrimPath = skelRootPrimPath.AppendChild(TfToken(TfMakeValidIdentifier(skelName)));
    UsdSkelSkeleton skeleton = UsdSkelSkeleton::Define(stage, skelPrimPath);

    UsdSkelBindingAPI::Apply(skeleton.GetPrim());

    // joint paths
    VtTokenArray jointTokens = { TfToken("Shoulder"), TfToken("Shoulder/Elbow"), TfToken("Shoulder/Elbow/Wrist") };

    UsdSkelTopology topo(jointTokens);
    std::string reason;
    if (!topo.Validate(&reason))
    {
        TF_WARN("Invalid skeleton topology: %s", reason.c_str());
        return;
    }
    skeleton.GetJointsAttr().Set(jointTokens);

    // bind transforms - provide the world space transform of each joint at bind time
    VtMatrix4dArray bindTransforms({ GfMatrix4d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -g_boneSize, 1),
                                     GfMatrix4d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1),
                                     GfMatrix4d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, g_boneSize, 1) });
    skeleton.GetBindTransformsAttr().Set(bindTransforms);

    // rest transforms - provides local space rest transforms of each joint
    // (serve as a fallback values for joints not overridden by an animation)
    VtMatrix4dArray restTransforms({ GfMatrix4d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1),
                                     GfMatrix4d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, g_boneSize, 1),
                                     GfMatrix4d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, g_boneSize, 1) });
    skeleton.GetRestTransformsAttr().Set(restTransforms);

    ///////////////
    // Skel Anim //
    ///////////////
    std::string animName("Anim");
    const SdfPath animPrimPath = skelRootPrimPath.AppendChild(TfToken(TfMakeValidIdentifier(animName)));

    // Create anim with a max elbow angle of -45 and a max wrist angle of 20
    // This function also binds ths animation to the skeleton
    createAndBindAnimForSkel(stage, animPrimPath, -45, 20);

    // Set the stage time-codes-per-second and end-time-code
    // NOTE: This is a stage-global operation, placed here for necessity.
    //       Ideally the end time code might take other animations in the stage into consideration.
    stage->SetTimeCodesPerSecond(24);
    stage->SetEndTimeCode(48);

    //////////////////
    // Skinned Mesh //
    //////////////////
    // Name
    std::string meshName("SkinnedMesh");
    // Path
    const SdfPath meshPrimPath = skelRootPrimPath.AppendChild(TfToken(omni::connect::core::getValidPrimName(meshName)));

    /***************

       point/vertex and joint map:

       2--j2--3
       |  |   |
       1--j1--4
       |  |   |
       0--j0--5

   ****************/
    VtVec3fArray points = {
        GfVec3f(-g_boneSize, 0.0, -g_boneSize), GfVec3f(-g_boneSize, 0.0, 0.0), GfVec3f(-g_boneSize, 0.0, g_boneSize),
        GfVec3f(g_boneSize, 0.0, g_boneSize),   GfVec3f(g_boneSize, 0.0, 0.0),  GfVec3f(g_boneSize, 0.0, -g_boneSize),
    };

    // Indices for each quad
    VtIntArray faceVertexIndices = { 0, 1, 4, 5, 1, 2, 3, 4 };

    // Face vertex count
    VtIntArray faceVertexCounts = { 4, 4 };

    // Vertex normals
    VtVec3fArray normals = { GfVec3f(0.0, 1.0, 0.0) };
    VtIntArray normalIndices = { 0, 0, 0, 0, 0, 0 };

    UsdGeomMesh mesh = omni::connect::core::definePolyMesh(
        stage,
        meshPrimPath,
        faceVertexCounts,
        faceVertexIndices,
        points,
        omni::connect::core::Vec3fPrimvarData(UsdGeomTokens->vertex, normals, normalIndices)
    );

    //////////////////////////////////////////
    // Apply the SkelBindingAPI to the mesh //
    //////////////////////////////////////////
    UsdSkelBindingAPI binding = UsdSkelBindingAPI::Apply(mesh.GetPrim());
    binding.CreateSkeletonRel().SetTargets(SdfPathVector({ skelPrimPath }));

    // Joint indices - vert to joint indices mapping
    VtIntArray jointIndices = { 0, 1, 2, 2, 1, 0 };
    binding.CreateJointIndicesPrimvar(false, 1).Set(VtValue(jointIndices));

    // Joint weights - vert to joint weight mapping
    VtFloatArray jointWeights = { 1, 1, 1, 1, 1, 1 };
    binding.CreateJointWeightsPrimvar(false, 1).Set(VtValue(jointWeights));

    // GeomBindTransform - For skinning to apply correctly set the bind-time world space transforms of the prim
    binding.CreateGeomBindTransformAttr().Set(VtValue(GfMatrix4d(1)));

    ///////////////////////////////////////////////////
    // Compute extents for the SkelRoot and Skeleton //
    ///////////////////////////////////////////////////
    UsdGeomBoundable::ComputeExtentFromPlugins(skelRoot, UsdTimeCode::Default(), &extent);
    skelRoot.CreateExtentAttr(VtValue(extent));

    UsdGeomBoundable::ComputeExtentFromPlugins(skelRoot, UsdTimeCode::Default(), &extent);
    skeleton.CreateExtentAttr(VtValue(extent));
}
