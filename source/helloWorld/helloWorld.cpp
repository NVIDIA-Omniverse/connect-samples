// SPDX-FileCopyrightText: Copyright (c) 2019-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//

/*###############################################################################
#
# This "helloworld" sample demonstrates how to:
#  * connect to an Omniverse server
#  * create a USD stage
#  * create a physics scene to define simulation parameters
#  * create a polygonal box and add it to the stage and make it a dynamic rigid
#  * create a cube and add it to the stage and make it a dynamic rigid
#  * create a quad and add it to the stage and make it a collider
#  * create a skinned skeleton, root, mesh, and anim
#  * upload an MDL material and its textures to an Omniverse server
#  * bind an MDL and USD Preview Surface material to the box
#  * add a light to the stage
#  * move and rotate the box with live updates
#  * create animations and animate a joint with live updates
#  * disconnect from an Omniverse server
#  *
#  * optional stuff:
#  *  print verbose Omniverse logs
#  *  open an existing stage and find a mesh to do live edits
#
###############################################################################*/

#include "exampleMaterial.h"
#include "exampleSkelMesh.h"

#include <omni/connect/core/CameraAlgo.h>
#include <omni/connect/core/Core.h>
#include <omni/connect/core/LightAlgo.h>
#include <omni/connect/core/LightCompatibility.h>
#include <omni/connect/core/Log.h>
#include <omni/connect/core/MaterialAlgo.h>
#include <omni/connect/core/MeshAlgo.h>
#include <omni/connect/core/PrimAlgo.h>
#include <omni/connect/core/StageAlgo.h>
#include <omni/connect/core/XformAlgo.h>

#include <omni/core/OmniInit.h>
#include <omni/log/ILog.h>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <map>
#include <math.h>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>


#ifdef _WIN32
#include <conio.h>
#include <filesystem>
namespace fs = std::filesystem;
#else
// Older GCC compilers don't have std::filesystem
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif
#include <OmniClient.h>

#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/kind/registry.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/payloads.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdUtils/pipeline.h>
#include <pxr/usd/usdUtils/sparseValueWriter.h>

// Physics includes, note that usdPhysics is now part of USD itself,
// in newer USD versions these includes will be replaced by pxr official usdPhysics schema
#if PXR_VERSION >= 2108
#include <pxr/usd/usdPhysics/collisionAPI.h>
#include <pxr/usd/usdPhysics/meshCollisionAPI.h>
#include <pxr/usd/usdPhysics/rigidBodyAPI.h>
#include <pxr/usd/usdPhysics/scene.h>
#else
#include <usdPhysics/collisionAPI.h>
#include <usdPhysics/meshCollisionAPI.h>
#include <usdPhysics/rigidBodyAPI.h>
#include <usdPhysics/scene.h>
#endif

#if PXR_VERSION >= 2208
#include <pxr/usd/usdGeom/primvarsAPI.h>
#endif

// Initialize the Omniverse application
OMNI_APP_GLOBALS("HelloWorld", "Omniverse HelloWorld Connector");

PXR_NAMESPACE_USING_DIRECTIVE

// Globals for Omniverse Connection and base Stage
static UsdStageRefPtr gStage;

static GfVec3f gDefaultRotation(0);
static GfVec3f gDefaultScale(1);

// Multiplatform array size
#define HW_ARRAY_COUNT(array) (sizeof(array) / sizeof(array[0]))

// Private tokens for building up SdfPaths. We recommend
// constructing SdfPaths via tokens, as there is a performance
// cost to constructing them directly via strings (effectively,
// a table lookup per path element). Similarly, any API which
// takes a token as input should use a predefined token
// rather than one created on the fly from a string.
// clang-format off
TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    (Camera)
    (DistantLight)
    (DomeLight)
    (Looks)
    (World)
    (diffuse_tint)
);
// clang-format on

// Shut down Omniverse connection
static void shutdownOmniverse()
{
    // Calling this prior to shutdown ensures that all pending live updates complete.
    omniClientLiveWaitForPendingUpdates();

    // The stage is a sophisticated object that needs to be destroyed properly.
    // Since gStage is a smart pointer we can just reset it
    gStage.Reset();
}

// Startup Omniverse
static bool startOmniverse(bool verbose)
{
    // Check that the core Omniverse frameworks started successfully
    OMNICONNECTCORE_INIT();
    if (!omni::connect::core::initialized())
    {
        return false;
    }

    // Set the retry behavior to limit retries so that invalid server addresses fail quickly
    omniClientSetRetries({ 1000, 500, 0 });

    auto log = omniGetLogWithoutAcquire();
    log->setLevel(verbose ? omni::log::Level::eVerbose : omni::log::Level::eInfo);

    // Handle connection errors. Note Connect SDK already provides logging for status changes,
    // so we only need to add any extra status handling.
    omniClientRegisterConnectionStatusCallback(
        nullptr,
        [](void* /* userData */, const char* url, OmniClientConnectionStatus status) noexcept
        {
            switch (status)
            {
                case eOmniClientConnectionStatus_Connecting:
                case eOmniClientConnectionStatus_Connected:
                {
                    // no need to inform the user as Connect SDK has handled this for us.
                    return;
                }
                default:
                {
                    // all other cases are failure states that means we cannot continue this sample program
                    OMNI_LOG_FATAL("Error connecting to Nucleus URL: <%s>  (OmniClientConnectionStatus: %d). Exiting.", url, status);
                    // We shouldn't just exit here it can cause crashes elsewhere because this is in a callback thread from the client
                }
            }
        }
    );

    return true;
}

// Create a new connection for this model in Omniverse, returns the created stage URL
static std::string createOmniverseModel(const std::string& destinationPath, bool doLiveEdit, const std::string& stageExtension)
{
    std::string stageUrl = destinationPath + "/helloworld" + (doLiveEdit ? ".live" : stageExtension);

    if (omni::connect::core::isOmniUri(destinationPath))
    {
        // Create the directory if it does not exist yet. Making sure we have access to it.
        if (!omni::connect::core::doesUriExist(destinationPath))
        {
            OmniClientResult createResult = eOmniClientResult_Error;
            omniClientWait(omniClientCreateFolder(
                destinationPath.c_str(),
                &createResult,
                [](void* userData, OmniClientResult result) noexcept
                {
                    if (userData)
                    {
                        OmniClientResult* deleteResultPtr = static_cast<OmniClientResult*>(userData);
                        *deleteResultPtr = result;
                    }
                }
            ));

            // This may be the first client call that accesses a server so check to see if we have connection or access issues
            if (createResult == eOmniClientResult_ErrorConnection)
            {
                OMNI_LOG_FATAL("Error connecting to Nucleus: <%s>", omniClientGetResultString(createResult));
                exit(1);
            }
            else if (!createResult == eOmniClientResult_Ok)
            {
                OMNI_LOG_FATAL("Error attempting to create USD stage: <%s>", omniClientGetResultString(createResult));
                exit(1);
            }
        }
        // If the directory exists, we need to test if we have access to it
        else if (!omni::connect::core::isUriWritable(destinationPath))
        {
            OMNI_LOG_FATAL("No write access to <%s>, exiting.", destinationPath.c_str());
            exit(1);
        }
        // Delete the existing file
        else if (omni::connect::core::doesUriExist(stageUrl))
        {
            OMNI_LOG_INFO("Waiting for %s to delete...", stageUrl.c_str());
            OmniClientResult deleteResult = eOmniClientResult_Error;
            omniClientWait(omniClientDelete(
                stageUrl.c_str(),
                &deleteResult,
                [](void* userData, OmniClientResult result) noexcept
                {
                    if (userData)
                    {
                        OmniClientResult* deleteResultPtr = static_cast<OmniClientResult*>(userData);
                        *deleteResultPtr = result;
                    }
                }
            ));
            if (!deleteResult == eOmniClientResult_Ok)
            {
                OMNI_LOG_FATAL("Error attempting to delete original stage: <%s>", omniClientGetResultString(deleteResult));
                exit(1);
            }
            OMNI_LOG_INFO("Deletion finished");
        }
    }

    // Create this file in Omniverse cleanly
    gStage = omni::connect::core::createStage(
        /* identifier */ stageUrl,
        /* defaultPrimName */ _tokens->World,
        /* upAxis */ UsdGeomTokens->y,
        /* linearUnits */ UsdGeomLinearUnits::centimeters
    );
    if (!gStage)
    {
        return std::string();
    }

    OMNI_LOG_INFO("New stage created: %s", stageUrl.c_str());

    // Redefine the defaultPrim as an Xform, as `createStage` authored a Scope
    UsdGeomXform defaultPrimXform = omni::connect::core::defineXform(gStage, gStage->GetDefaultPrim().GetPath());

    if (!defaultPrimXform)
    {
        std::string pathStr = gStage->GetDefaultPrim().GetPath().GetAsString();
        OMNI_LOG_WARN("Failure to redefine default prim - %s as Xform. Exiting.", pathStr.c_str());
        exit(1);
    }

    // Set the default prim as an assembly to support using component references
    pxr::UsdModelAPI(defaultPrimXform).SetKind(pxr::KindTokens->assembly);

    return stageUrl;
}

static void saveStage(UsdStagePtr stage, const char* comment)
{
    OMNI_LOG_INFO("Adding checkpoint comment <%s> to stage <%s>", comment, stage->GetRootLayer()->GetIdentifier().c_str());
    omni::connect::core::saveStage(stage, comment);
}

static void createPhysicsScene()
{
    const SdfPath& defaultPrimPath = gStage->GetDefaultPrim().GetPath();
    std::string sceneName("physicsScene");

    const SdfPath scenePrimPath = defaultPrimPath.AppendChild(TfToken(omni::connect::core::getValidPrimName(sceneName)));

    // Create physics scene, note that we dont have to specify gravity
    // the default value is derived based on the scene up Axis and meters per unit.
    // Hence in this case the gravity would be (0.0, -981.0, 0.0) since we have
    // defined the Y up-axis and we are having a scene in centimeters.
    UsdPhysicsScene::Define(gStage, scenePrimPath);
}

static void enablePhysics(const UsdPrim& prim, bool dynamic)
{
    if (dynamic)
    {
        // Make the cube a physics rigid body dynamic
        UsdPhysicsRigidBodyAPI::Apply(prim);
    }

    // Add collision
    UsdPhysicsCollisionAPI collisionAPI = UsdPhysicsCollisionAPI::Apply(prim);
    if (!collisionAPI)
    {
        OMNI_LOG_ERROR("Failed to apply UsdPhysics.CollisionAPI, check that the UsdPhysics plugin is located in the USD plugins folders");
    }

    if (prim.IsA<UsdGeomMesh>())
    {
        UsdPhysicsMeshCollisionAPI meshCollisionAPI = UsdPhysicsMeshCollisionAPI::Apply(prim);
        if (dynamic)
        {
            // set mesh approximation to convexHull for dynamic meshes
            meshCollisionAPI.CreateApproximationAttr().Set(UsdPhysicsTokens->convexHull);
        }
        else
        {
            // set mesh approximation to none - triangle mesh as is will be used
            meshCollisionAPI.CreateApproximationAttr().Set(UsdPhysicsTokens->none);
        }
    }
}

static void createDynamicCube(double size)
{
    // Create the geometry under the default prim
    const SdfPath& defaultPrimPath = gStage->GetDefaultPrim().GetPath();
    std::string cubeName("cube");
    const SdfPath cubePrimPath = defaultPrimPath.AppendChild(TfToken(omni::connect::core::getValidPrimName(cubeName)));
    UsdGeomCube cube = UsdGeomCube::Define(gStage, cubePrimPath);

    if (!cube)
    {
        return;
    }

    // Move it up
    cube.AddTranslateOp(UsdGeomXformOp::PrecisionFloat).Set(GfVec3f(65.0f, 300.0f, 65.0f));

    cube.GetSizeAttr().Set(size);
    VtArray<GfVec3f> extent;
    cube.GetExtentAttr().Get(&extent);
    cube.CreateExtentAttr(VtValue(size * 0.5 * extent));

    enablePhysics(cube.GetPrim(), true);

    // Make the kind a component to support the assembly/component selection hierarchy
    pxr::UsdModelAPI(cube.GetPrim()).SetKind(pxr::KindTokens->component);

    // Commit the changes to the USD
    saveStage(gStage, "Add a dynamics cube");
    omniClientLiveProcess();
}

// Create a simple quad in USD with normals and add a collider
static void createQuad(double size)
{
    // Name
    std::string quadName("quad");

    // Add face vertex count
    VtArray<int> faceVertexCounts = { 4 };
    VtArray<int> faceVertexIndices = { 0, 1, 2, 3 };
    VtArray<GfVec3f> points = {
        GfVec3f(-size, 0.0, -size),
        GfVec3f(-size, 0.0, size),
        GfVec3f(size, 0.0, size),
        GfVec3f(size, 0.0, -size),
    };

    VtArray<GfVec3f> normals = { GfVec3f(0.0, 1.0, 0.0), GfVec3f(0.0, 1.0, 0.0), GfVec3f(0.0, 1.0, 0.0), GfVec3f(0.0, 1.0, 0.0) };

    omni::connect::core::Vec3fPrimvarData normalsPrimvarData = omni::connect::core::Vec3fPrimvarData(UsdGeomTokens->vertex, normals);
    normalsPrimvarData.index();

    // Create the geometry under the default prim
    UsdGeomMesh mesh = omni::connect::core::definePolyMesh(
        gStage->GetDefaultPrim(),
        omni::connect::core::getValidPrimName(quadName),
        faceVertexCounts,
        faceVertexIndices,
        points,
        normalsPrimvarData
    );

    if (!mesh)
    {
        return;
    }

    // set is as a static triangle mesh
    enablePhysics(mesh.GetPrim(), false);

    // Make the kind a component to support the assembly/component selection hierarchy
    pxr::UsdModelAPI(mesh.GetPrim()).SetKind(pxr::KindTokens->component);

    createAndBindMaterial(mesh.GetPrim(), "QuadMat", GfVec3f(0.025f, 0.025f, 0.025f), nullptr, nullptr, nullptr, 1.0f, 0.04f);

    // Commit the changes to the USD
    saveStage(gStage, "Add box and nothing else");
    omniClientLiveProcess();
}

// Create a simple box in USD with normals and UV information

// Box mesh data
// clang-format off
double gVal = 50.0;
int gBoxVertexIndices[] = {0, 1, 2, 1, 3, 2, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11, 12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23};
double gBoxNormals[][3] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}};
int gBoxNormalIndices[] = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5};
double gBoxPoints[][3] = {{gVal, -gVal, -gVal}, {-gVal, -gVal, -gVal}, {gVal, gVal, -gVal}, {-gVal, gVal, -gVal}, {gVal, gVal, gVal}, {-gVal, gVal, gVal}, {-gVal, -gVal, gVal}, {gVal, -gVal, gVal}, {gVal, -gVal, gVal}, {-gVal, -gVal, gVal}, {-gVal, -gVal, -gVal}, {gVal, -gVal, -gVal}, {gVal, gVal, gVal}, {gVal, -gVal, gVal}, {gVal, -gVal, -gVal}, {gVal, gVal, -gVal}, {-gVal, gVal, gVal}, {gVal, gVal, gVal}, {gVal, gVal, -gVal}, {-gVal, gVal, -gVal}, {-gVal, -gVal, gVal}, {-gVal, gVal, gVal}, {-gVal, gVal, -gVal}, {-gVal, -gVal, -gVal}};
float gBoxUV[][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
int gBoxUVIndices[] = {0, 1, 3, 2, 0, 1, 3, 2, 0, 1, 3, 2, 0, 1, 3, 2, 0, 1, 3, 2, 0, 1, 3, 2};
// clang-format on

static UsdGeomMesh createBox(int boxNumber = 0)
{
    // Name
    std::string boxName("box-");
    boxName.append(std::to_string(boxNumber));

    // Face vertex count
    VtArray<int> faceVertexCounts;
    faceVertexCounts.resize(12); // 2 Triangles per face * 6 faces
    std::fill(faceVertexCounts.begin(), faceVertexCounts.end(), 3); // Triangle

    // Calculate indices for each triangle
    int num_indices = HW_ARRAY_COUNT(gBoxVertexIndices); // 2 Triangles per face * 3 Vertices per Triangle * 6 Faces
    VtArray<int> faceVertexIndices;
    faceVertexIndices.resize(num_indices);
    for (int i = 0; i < num_indices; i++)
    {
        faceVertexIndices[i] = gBoxVertexIndices[i];
    }

    // all of the vertices
    int num_vertices = HW_ARRAY_COUNT(gBoxPoints);
    VtArray<GfVec3f> points;
    points.resize(num_vertices);
    for (int i = 0; i < num_vertices; i++)
    {
        points[i] = GfVec3f(gBoxPoints[i][0], gBoxPoints[i][1], gBoxPoints[i][2]);
    }

    // normals
    int num_normals = HW_ARRAY_COUNT(gBoxNormals);
    VtArray<GfVec3f> normals;
    normals.resize(num_normals);
    for (int i = 0; i < num_normals; i++)
    {
        normals[i] = GfVec3f((float)gBoxNormals[i][0], (float)gBoxNormals[i][1], (float)gBoxNormals[i][2]);
    }

    int num_normal_indices = HW_ARRAY_COUNT(gBoxNormalIndices);
    VtArray<int> normalIndices;
    normalIndices.resize(num_normal_indices);
    for (int i = 0; i < num_normal_indices; i++)
    {
        normalIndices[i] = gBoxNormalIndices[i];
    }

    // UV (st)
    int uv_count = HW_ARRAY_COUNT(gBoxUV);
    VtVec2fArray uvs;
    uvs.resize(uv_count);
    for (int i = 0; i < uv_count; ++i)
    {
        uvs[i].Set(gBoxUV[i]);
    }

    int num_uv_indices = HW_ARRAY_COUNT(gBoxUVIndices);
    VtArray<int> uvIndices;
    uvIndices.resize(num_uv_indices);
    for (int i = 0; i < num_uv_indices; i++)
    {
        uvIndices[i] = gBoxUVIndices[i];
    }

    // Create the geometry under the default prim
    UsdGeomMesh mesh = omni::connect::core::definePolyMesh(
        gStage->GetDefaultPrim(),
        omni::connect::core::getValidPrimName(boxName),
        faceVertexCounts,
        faceVertexIndices,
        points,
        omni::connect::core::Vec3fPrimvarData(UsdGeomTokens->vertex, normals, normalIndices),
        omni::connect::core::Vec2fPrimvarData(UsdGeomTokens->vertex, uvs, uvIndices),
        omni::connect::core::Vec3fPrimvarData(UsdGeomTokens->constant, { { 0.463f, 0.725f, 0.0f } }) // displayColor
    );

    if (!mesh)
    {
        return mesh;
    }

    // Set the display name as the preferred name: {box-boxNumber}
    omni::connect::core::setDisplayName(mesh.GetPrim(), boxName);

    // Move it up and rotate
    omni::connect::core::setLocalTransform(
        mesh.GetPrim(),
        GfVec3d(0.0f, 100.0f, 0.0f), /* translation */
        GfVec3d(0.0), /* pivot */
        GfVec3f(20.0, 0.0, 20.0), /* rotation */
        omni::connect::core::RotationOrder::eXyz,
        gDefaultScale
    );

    // Make the cube a physics rigid body dynamic
    enablePhysics(mesh.GetPrim(), true);

    // Make the kind a component to support the assembly/component selection hierarchy
    pxr::UsdModelAPI(mesh.GetPrim()).SetKind(pxr::KindTokens->component);

    // Commit the changes to the USD
    gStage->Save();
    omniClientLiveProcess();

    return mesh;
}

// Opens an existing stage and finds the first UsdGeomMesh
static UsdGeomMesh findGeomMesh(const std::string& existingStage)
{
    // Open this file from Omniverse
    gStage = UsdStage::Open(existingStage);
    if (!gStage)
    {
        OMNI_LOG_ERROR("Failure to open stage in Omniverse: %s", existingStage.c_str());
        return UsdGeomMesh();
    }

    OMNI_LOG_INFO("Existing stage opened: %s", existingStage.c_str());

    if (UsdGeomTokens->y != UsdGeomGetStageUpAxis(gStage))
    {
        OMNI_LOG_WARN("Stage is not Y-up so live xform edits will be incorrect.  Stage is %s-up", UsdGeomGetStageUpAxis(gStage).GetText());
    }

    // Traverse the stage and return the first UsdGeomMesh we find
    auto range = gStage->Traverse();
    for (const auto& node : range)
    {
        if (node.IsA<UsdGeomMesh>())
        {
            OMNI_LOG_INFO("Found UsdGeomMesh: %s", node.GetName().GetText());
            return UsdGeomMesh(node);
        }
    }

    // No UsdGeomMesh found in stage (what kind of stage is this anyway!?)
    OMNI_LOG_ERROR("No UsdGeomMesh found in stage: %s", existingStage.c_str());
    return UsdGeomMesh();
}

// Upload a material and its textures to the Omniverse Server
static void uploadReferences(const std::string& destinationPath)
{
    std::string matPath = destinationPath + "/Materials";
    std::string propPath = destinationPath + "/Props";

    if (!fs::is_directory("resources/Props"))
    {
        OMNI_LOG_FATAL("Could not find the \"resources/Props\" folder, run HelloWorld with the \"run_hello_world\" script from the root folder.");
        _Exit(1);
    }

    // Delete the old version of this folder on Omniverse and wait for the operation to complete, then upload
    omniClientWait(omniClientDelete(matPath.c_str(), nullptr, nullptr));
    omniClientWait(omniClientCopy("resources/Materials", matPath.c_str(), nullptr, nullptr));
    // Referenced Props
    omniClientWait(omniClientDelete(propPath.c_str(), nullptr, nullptr));
    omniClientWait(omniClientCopy("resources/Props", propPath.c_str(), nullptr, nullptr));
}


// Create references and modify an OmniPBR material
void createReferenceAndPayload()
{
    // The referenced prop is in /Props/Box/box.usda
    const SdfPath& defaultPrimPath = gStage->GetDefaultPrim().GetPath();

    // Create a reference, enable physics and give it some angular velocity
    std::string xformName = omni::connect::core::getValidPrimName("Box_Reference");
    UsdGeomXform xform = omni::connect::core::defineXform(gStage, defaultPrimPath.AppendChild(TfToken(xformName)));
    if (!xform)
    {
        OMNI_LOG_FATAL("Failure to create xform - %s", xformName.c_str());
        exit(1);
    }
    UsdPrim xformPrim = xform.GetPrim();
    xformPrim.GetReferences().AddReference("./Props/Box/box.usda");
    UsdPrim meshPrim = gStage->GetPrimAtPath(xformPrim.GetPath().AppendChild(TfToken("box")));
    enablePhysics(meshPrim, true);
    UsdPhysicsRigidBodyAPI(meshPrim).CreateAngularVelocityAttr(VtValue(GfVec3f(0, 1000, 0)));
    omni::connect::core::setLocalTransform(
        xformPrim,
        GfVec3d(200, 100, -200),
        /* pivot */ GfVec3d(0.0),
        /* rotation */ GfVec3f(3, 0, 8),
        omni::connect::core::RotationOrder::eXyz,
        GfVec3f(0.5)
    );

    // Create a payload, enable physics and give it some angular velocity
    xformName = omni::connect::core::getValidPrimName("Box_Payload");
    xform = omni::connect::core::defineXform(gStage, defaultPrimPath.AppendChild(TfToken(xformName)));
    if (!xform)
    {
        OMNI_LOG_FATAL("Failure to create xform - %s", xformName.c_str());
        exit(1);
    }
    xformPrim = xform.GetPrim();
    xformPrim.GetPayloads().AddPayload("./Props/Box/box.usda");
    meshPrim = gStage->GetPrimAtPath(xformPrim.GetPath().AppendChild(TfToken("box")));
    enablePhysics(meshPrim, true);
    UsdPhysicsRigidBodyAPI(meshPrim).CreateAngularVelocityAttr(VtValue(GfVec3f(-1000, 0, 0)));
    omni::connect::core::setLocalTransform(
        xformPrim,
        GfVec3d(-200, 180, 200),
        /* pivot */ GfVec3d(0.0),
        /* rotation */ GfVec3f(-4, 90, 8),
        omni::connect::core::RotationOrder::eXyz,
        GfVec3f(0.5)
    );

    // Modify the payload's material in Box/Looks/Fieldstone
    SdfPath materialPrimPath = defaultPrimPath.AppendChild(TfToken("Box_Payload")).AppendChild(_tokens->Looks).AppendChild(TfToken("Fieldstone"));
    UsdShadeMaterial materialPrim = UsdShadeMaterial::Get(gStage, materialPrimPath);
    omni::connect::core::createMdlShaderInput(materialPrim, _tokens->diffuse_tint, VtValue(GfVec3f(1, 0.1, 0)), SdfValueTypeNames->Color3f);

    // This save will emit a warning about "Upgrading crate file", this is OK
    // Commit the changes to the USD
    saveStage(gStage, "Add Reference, Payload, and modified OmniPBR");
}

// Create a light source in the scene.
static void createDistantLight()
{
    // Construct path for the light under the default prim
    const SdfPath& defaultPrimPath = gStage->GetDefaultPrim().GetPath();
    const SdfPath lightPath = defaultPrimPath.AppendChild(_tokens->DistantLight);
    UsdLuxDistantLight newLight = UsdLuxDistantLight::Define(gStage, lightPath);

    // Set the UsdLuxLight attributes.
    omni::connect::core::createDistantAngleAttr(newLight, 0.53f);

    // Grab the LuxLightAPI so we can set generic light attributes
    UsdLuxLightAPI lightApi = UsdLuxLightAPI(newLight);

    GfVec3f color(1.0f, 1.0f, 0.745f);
    omni::connect::core::createColorAttr(lightApi, color);
    omni::connect::core::createIntensityAttr(lightApi, 500.0f);

    // Grab the prim so we can set the xform and kind
    UsdPrim lightPrim = newLight.GetPrim();

    // Rotate the distant light
    // There are multiple approaches to setting transforms - we'll use the GfRotation API in this case,
    // but you could also use the TRS method seen in other functions here, or by constructing a 4x4 matrix.
    // Note that these methods work across all prims, not just lights.

    // Grab the transform of the object you want to reorient
    GfTransform lightXform = omni::connect::core::getLocalTransform(lightPrim);

    // Create a rotation
    GfRotation rotation = GfRotation(GfVec3d::XAxis(), /* angle */ 139) * GfRotation(GfVec3d::YAxis(), 44) * GfRotation(GfVec3d::ZAxis(), 190);

    // This quaternion is equivalent to the above
    // GfRotation rotation = GfRotation(GfQuatd(0.32124, -0.20638, 0.85372, 0.35405));

    // Apply the rotation to the xform
    lightXform.SetRotation(rotation);

    // Last, set the transform to the prim
    omni::connect::core::setLocalTransform(lightPrim, lightXform);

    // Alternatively you could use this Connect SDK method to do all of the above in a single call:
    // omni::connect::core::setLocalTransform(lightPrim, GfVec3d(0.0f), /* pivot */ GfVec3d(0.0), /* rotation */ GfVec3f(139, 44, 190),
    // omni::connect::core::RotationOrder::eXyz, gDefaultScale);

    // Or create a 4x4 matrix, which provides a few other tools
    // GfMatrix4d matrix = GfMatrix4d();
    // matrix.SetRotate(rotation);
    // omni::connect::core::setLocalTransform(lightPrim, matrix);

    // Make the kind a component to support the assembly/component selection hierarchy
    pxr::UsdModelAPI(lightPrim).SetKind(pxr::KindTokens->component);

    // Commit the changes to the USD
    saveStage(gStage, "Add a distant light to stage");
    omniClientLiveProcess();
}

// Create a light source in the scene.
static void createDomeLight(const char* texturePath)
{
    // Construct path for the light under the default prim
    const SdfPath& defaultPrimPath = gStage->GetDefaultPrim().GetPath();
    const SdfPath lightPath = defaultPrimPath.AppendChild(_tokens->DomeLight);

    // Define dome light
    UsdLuxDomeLight newLight = omni::connect::core::defineDomeLight(gStage, lightPath, 900.0f, texturePath, UsdLuxTokens->latlong);

    // Set rotation on domelight
    UsdGeomXformable xForm = newLight;
    UsdGeomXformOp rotateOp;
    GfVec3d rotXYZ(270, 270, 0);
    rotateOp = xForm.AddXformOp(UsdGeomXformOp::TypeRotateXYZ, UsdGeomXformOp::Precision::PrecisionDouble);
    rotateOp.Set(rotXYZ);

    // Make the kind a component to support the assembly/component selection hierarchy
    pxr::UsdModelAPI(newLight.GetPrim()).SetKind(pxr::KindTokens->component);

    // Commit the changes to the USD
    saveStage(gStage, "Add a dome light to stage");
    omniClientLiveProcess();
}


// Create cylinders. This is for demonstrating omni::connect::core::getValidChildNames and omni::connect::core::getValidPrimName
static void createCylinders(double size)
{
    std::string xformName("1Cylinders");
    UsdPrim defaultPrim = gStage->GetDefaultPrim();

    // Note we use omni::connect::core::getValidPrimName to get a valid prim name - This will prepend a underscore in front of 1 - _1Cylinder
    UsdGeomXform xform = omni::connect::core::defineXform(defaultPrim, omni::connect::core::getValidPrimName(xformName));

    if (!xform)
    {
        return;
    }

    std::vector<std::string> cylinderNames = { "1cylinder", "1cylinder", "1cylinder" };

    TfTokenVector validNames = omni::connect::core::getValidChildNames(xform.GetPrim(), cylinderNames);

    for (size_t idx = 0; idx < validNames.size(); ++idx)
    {
        SdfPath cylinderPath = xform.GetPrim().GetPath().AppendChild(TfToken(validNames[idx]));
        UsdGeomCylinder cylinder = UsdGeomCylinder::Define(gStage, cylinderPath);

        if (!cylinder)
        {
            return;
        }

        UsdPrim cylinderPrim = cylinder.GetPrim();
        // Set the display name as the preferred name
        omni::connect::core::setDisplayName(cylinderPrim, cylinderNames[idx]);

        GfTransform cylinderXform;
        // Scale it
        cylinderXform.SetScale(GfVec3d(size * (idx + 1)));
        // Move it up
        cylinderXform.SetTranslation(GfVec3d(350, 100.0 * idx, 350));
        omni::connect::core::setLocalTransform(cylinderPrim, cylinderXform);
    }

    saveStage(gStage, "Add cylinders");
}


static void createCamera()
{
    // Construct path for the camera under the default prim
    const SdfPath& defaultPrimPath = gStage->GetDefaultPrim().GetPath();
    const SdfPath cameraPath = defaultPrimPath.AppendChild(_tokens->Camera);

    // GfCamera is a container for camera attributes.
    // The connect sdk defineCamera function takes it as an argument.
    GfCamera gfCam = GfCamera();

    // Put the camera about a 1000 units from the origin, and focus on
    // the textured cube we created in a previous function
    gfCam.SetFocusDistance(1000.0f);
    gfCam.SetFocalLength(35.0f);
    gfCam.SetFStop(0.5f);
    gfCam.SetHorizontalAperture(15.0f);

    // Define the camera
    UsdGeomCamera newCamera = omni::connect::core::defineCamera(gStage, cameraPath, gfCam);

    // We could configure the xform in the GfCamera, but we can also do so with:
    UsdPrim cameraPrim = newCamera.GetPrim();
    omni::connect::core::setLocalTransform(
        cameraPrim,
        /* translation */ GfVec3d(932.84f, 96.0f, -453.57f),
        /* pivot */ GfVec3d(0.0f),
        /* rotation */ GfVec3f(-178.52f, 66.03f, -180.0f),
        omni::connect::core::RotationOrder::eXyz,
        gDefaultScale
    );
}

// Create an empty folder, just as an example.
static void createEmptyFolder(const std::string& emptyFolderPath)
{
    OMNI_LOG_INFO("Waiting to create a new folder: %s ...", emptyFolderPath.c_str());

    OmniClientResult localResult;
    localResult = Count_eOmniClientResult;

    omniClientWait(omniClientCreateFolder(
        emptyFolderPath.c_str(),
        &localResult,
        [](void* userData, OmniClientResult result) noexcept
        {
            auto returnResult = static_cast<OmniClientResult*>(userData);
            *returnResult = result;
        }
    ));

    OMNI_LOG_INFO("finished [%s]", omniClientGetResultString(localResult));
}

std::string getline()
{
#ifndef _WIN32
    // On Linux we seem to require an extra `get` in order to pause for user input,
    // whereas running the same on Windows would "steal" the first character of the
    // input message...
    std::cin.get();
#endif
    std::string line;
    std::getline(std::cin, line);

    return line;
}

char inputChar()
{
#ifdef _WIN32
    char achar = _getch();
#else
    char achar = getchar();
#endif
    return achar;
}

// Perform a live edit on the box
static void liveEdit(UsdGeomMesh& meshIn, std::string stageUrl)
{
    double angle = 0;
    std::srand(std::time(0));

    constexpr const char kLoopText[] =
        "Enter 't' to transform,\n"
        "'s' to create anim,\n"
        "'a' to stream anim,\n"
        "'m' to msg channel,\n"
        "'l' to leave channel,\n"
        "'q' to quit";

    // Process any updates that may have happened to the stage from another client
    omniClientLiveProcess();
    OMNI_LOG_INFO("Begin Live Edit on %s\n%s\n", meshIn.GetPath().GetText(), kLoopText);

    // Join a message channel to communicate text messages between clients
    OmniClientRequestId joinRequestId = omniClientJoinChannel(
        (stageUrl + ".__omni_channel__").c_str(),
        nullptr,
        [](void* /* userData */, OmniClientResult /* result */, OmniClientChannelEvent eventType, const char* /* from */, OmniClientContent* content
        ) noexcept
        {
            std::string eventTypeStr;
            switch (eventType)
            {
                case eOmniClientChannelEvent_Error:
                    eventTypeStr = "ERROR";
                    break;
                case eOmniClientChannelEvent_Message:
                    eventTypeStr = "MESSAGE";
                    break;
                case eOmniClientChannelEvent_Hello:
                    eventTypeStr = "HELLO";
                    break;
                case eOmniClientChannelEvent_Join:
                    eventTypeStr = "JOIN";
                    break;
                case eOmniClientChannelEvent_Left:
                    eventTypeStr = "LEFT";
                    break;
                case eOmniClientChannelEvent_Deleted:
                    eventTypeStr = "DELETED";
                    break;
                default:
                    break;
            }
            // \todo: setup unique logging channels?
            OMNI_LOG_INFO("Channel event: %s", eventTypeStr.c_str());

            if (eventType == eOmniClientChannelEvent_Message)
            {
                // Assume that this is an ASCII message from another client
                std::string messageText((char*)content->buffer, content->size);
                OMNI_LOG_INFO("Channel message received: %s", messageText.c_str());
            }
        }
    );

    bool wait = true;
    while (wait)
    {
        char nextCommand = inputChar();

        // Process any updates that may have happened to the stage from another client
        omniClientLiveProcess();

        switch (nextCommand)
        {
            case 't':
            {
                // Increase the angle
                angle += 15;
                if (angle >= 360)
                {
                    angle = 0;
                }

                double radians = angle * M_PI / 180.0;
                double x = sin(radians) * 100;
                double y = cos(radians) * 100;

                GfVec3d position(0);
                GfVec3d pivot(0);
                GfVec3f rotXYZ(0);
                omni::connect::core::RotationOrder rotationOrder;
                GfVec3f scale(1);

                UsdPrim meshPrim = meshIn.GetPrim();

                omni::connect::core::getLocalTransformComponents(meshPrim, position, pivot, rotXYZ, rotationOrder, scale);

                // Move/Rotate the existing position/rotation - this works for Y-up stages
                position += GfVec3d(x, 0, y);
                rotXYZ = GfVec3f(rotXYZ[0], angle, rotXYZ[2]);

                omni::connect::core::setLocalTransform(meshPrim, position, pivot, rotXYZ, rotationOrder, scale);

                OMNI_LOG_INFO(
                    "Setting pos: ( %f, %f, %f ) and rot: ( %f, %f, %f )",
                    position[0],
                    position[1],
                    position[2],
                    rotXYZ[0],
                    rotXYZ[1],
                    rotXYZ[2]
                );
                // Commit the change to USD
                omniClientLiveProcess();
                break;
            }
            case 's':
            {
                // Create a new skeletal animation and bind it to the skeleton
                static int animCounter = 0;
                // Generate a new animation prim path
                const SdfPath& defaultPrimPath = gStage->GetDefaultPrim().GetPath();
                std::string animName("randomLiveAnim_");
                animName.append(std::to_string(animCounter++));
                const SdfPath animPrimPath = defaultPrimPath.AppendChild(TfToken(omni::connect::core::getValidPrimName(animName)));

                // Create a new animation prim with random elbow and wrist max angles, assign to skeleton
                int elbowAngle = 90 - (1 + std::rand() / ((RAND_MAX + 1u) / 180));
                int wristAngle = 45 - (1 + std::rand() / ((RAND_MAX + 1u) / 90));
                OMNI_LOG_INFO("Create animation <%s> with max elbow/wrist angles <%i/%i>", animPrimPath.GetAsString().c_str(), elbowAngle, wristAngle);
                createAndBindAnimForSkel(gStage, animPrimPath, (double)elbowAngle, (double)wristAngle);

                // Commit the change to USD
                omniClientLiveProcess();
                break;
            }
            case 'a':
            {
                // Update the elbow translation at 30 FPS for 2 seconds to simulate a Connector tweaking or streaming a skeleton
                using namespace std::chrono_literals;
                const auto kTimerPeriod = 33.3ms;
                const int kFrames = 60;
                const char kScrollWheel[4] = { '/', '-', '\\', '|' };
                std::cout << "Streaming elbow joint transforms to the skeleton's animation at time=0:  ";
                for (int i = 0; i < kFrames; ++i)
                {
                    double intermediateAngle = 2.0 * (double)(i + 1) * M_PI / (double)kFrames;
                    double displacement = sin(intermediateAngle) * 100.0;
                    std::cout << "\b" << kScrollWheel[i % 4];
                    fflush(stdout);
                    setElbowRelativeXTranslation(gStage, displacement);
                    // Commit the change to USD
                    omniClientLiveProcess();

                    // A real application would be doing something else rather than blocking the main thread here
                    std::this_thread::sleep_for(kTimerPeriod);
                }
                std::cout << std::endl;
                break;
            }
            case 'm':
            {
                if (kInvalidRequestId != joinRequestId)
                {
                    OMNI_LOG_INFO("Enter a channel message:");

                    std::string message = getline();

                    size_t msgLen = message.length() + 1;
                    // omniClientSendMessage will call the OmniClientContent::free() function when finished with the buffer
                    char* messageBuffer = (char*)malloc(msgLen);
                    std::strncpy(messageBuffer, message.c_str(), msgLen);
                    // sanitize for static analysis
                    messageBuffer[message.length()] = '\0';

                    OmniClientContent content = { /* buffer */ (void*)messageBuffer,
                                                  /* length */ msgLen,
                                                  /* free function*/
                                                  [](void* buf) noexcept
                                                  {
                                                      free(buf);
                                                  } };

                    omniClientSendMessage(
                        joinRequestId,
                        &content,
                        nullptr,
                        [](void* /* userData */, OmniClientResult /* result */) noexcept
                        {
                        }
                    );
                }
                else
                {
                    OMNI_LOG_INFO("The message channel is disconnected.");
                }

                break;
            }
            case 'l':
            {
                OMNI_LOG_INFO("Leaving message channel.");
                omniClientStop(joinRequestId);
                joinRequestId = kInvalidRequestId;
                break;
            }
            // escape or 'q'
            case 27:
            case 'q':
                wait = false;
                OMNI_LOG_INFO("Live Edit complete\n");
                break;
            default:
                OMNI_LOG_INFO(kDefaultChannel, kLoopText);
        }
    }
}

// Print the command line arguments help
static void printCmdLineArgHelp()
{
    OMNI_LOG_INFO(
        "\nUsage: samples [options]\n"
        "  options:\n"
        "    -h, --help                    Print this help\n"
        "    -l, --live                    Allow the user to continue modifying the stage live after creating (with the 't' key)\n"
        "    -p, --path dest_stage_folder  Alternate destination stage path folder [default: omniverse://localhost/Users/test]\n"
        "    -e, --existing path_to_stage  Open an existing stage and perform live transform edits (full omniverse URL)\n"
        "    -v, --verbose                 Show the verbose Omniverse logging\n"
        "\n\nExamples:\n"
        " * create a stage on the localhost server at /Projects/HelloWorld/helloworld.usd\n"
        "    > samples -p omniverse://localhost/Projects/HelloWorld\n"
        "\n * live edit a stage on the localhost server at /Projects/LiveEdit/livestage.usd\n"
        "    > samples -e omniverse://localhost/Projects/LiveEdit/livestage.usd\n"
    );
}


// Main Application
int main(int argc, char* argv[])
{
    bool doLiveEdit = false;
    std::string existingStage;
    std::string destinationPath = "";
    std::string finalCheckpointComment = "HelloWorld sample completed";
    std::string stageExtension(".usd");
    UsdGeomMesh boxMesh;

    // check for verbose flag
    bool verbose = false;
    for (int x = 1; x < argc; x++)
    {
        if (strcmp(argv[x], "-v") == 0 || strcmp(argv[x], "--verbose") == 0)
        {
            verbose = true;
        }
    }

    // Startup Omniverse
    if (!startOmniverse(verbose))
    {
        exit(1);
    }

    // Process the arguments, if any
    for (int x = 1; x < argc; x++)
    {
        if (strcmp(argv[x], "-h") == 0 || strcmp(argv[x], "--help") == 0)
        {
            printCmdLineArgHelp();
            return 0;
        }
        else if (strcmp(argv[x], "-a") == 0 || strcmp(argv[x], "--usda") == 0)
        {
            stageExtension = ".usda";
        }
        else if (strcmp(argv[x], "-l") == 0 || strcmp(argv[x], "--live") == 0)
        {
            doLiveEdit = true;
        }
        else if ((strcmp(argv[x], "-p") == 0 || strcmp(argv[x], "--path") == 0) && argc > x)
        {
            if (x == argc - 1)
            {
                OMNI_LOG_ERROR("ERROR: Missing an Omniverse folder URL to create the stage.");
                printCmdLineArgHelp();
                return -1;
            }
            destinationPath = std::string(argv[++x]);
            if (!omni::connect::core::isOmniUri(destinationPath))
            {
                OMNI_LOG_WARN(
                    "This is not an Omniverse Nucleus URL: %s\n"
                    "Correct Omniverse URL format is: omniverse://server_name/Path/To/Example/Folder\n"
                    "Allowing program to continue because file paths may be provided in the form: C:/Path/To/Stage",
                    destinationPath.c_str()
                );
            }
        }
        else if (strcmp(argv[x], "-v") == 0 || strcmp(argv[x], "--verbose") == 0)
        {
            // this was handled in the pre-process loop
            continue;
        }
        else if (strcmp(argv[x], "-e") == 0 || strcmp(argv[x], "--existing") == 0)
        {
            doLiveEdit = true;
            if (x == argc - 1)
            {
                OMNI_LOG_ERROR("ERROR: Missing an Omniverse URL to the stage to edit.");
                printCmdLineArgHelp();
                return -1;
            }
            existingStage = std::string(argv[++x]);
            if (!omni::connect::core::isOmniUri(existingStage))
            {
                OMNI_LOG_WARN(
                    "This is not an Omniverse Nucleus URL: %s\n"
                    "Correct Omniverse URL format is: omniverse://server_name/Path/To/Example/Folder\n"
                    "Allowing program to continue because file paths may be provided in the form: C:/Path/To/Stage",
                    existingStage.c_str()
                );
            }
            else if (!omni::connect::core::doesUriExist(existingStage))
            {
                OMNI_LOG_ERROR(
                    "Provided stage URL does not exist - %s\n"
                    "Maybe use -p flag to create a new sample USD?",
                    existingStage.c_str()
                );
                return -1;
            }
            else if (!omni::connect::core::isUriWritable(existingStage))
            {
                OMNI_LOG_WARN(
                    "Provided stage URL is not writable - %s\n"
                    "Allowing program to continue, all changes will be discarded when exiting.",
                    existingStage.c_str()
                );
            }
        }
        else
        {
            OMNI_LOG_ERROR("Unrecognized option: %s", argv[x]);
        }
    }

    // Find the correct user folder on Nucleus (if the path is was specified)
    if (existingStage.empty() && destinationPath.empty())
    {
        OMNI_LOG_INFO("No output path specified, so checking the localhost Users folder for a valid username");
        std::string userFolder = "omniverse://localhost/Users";
        std::string username = omni::connect::core::getUser(userFolder);
        destinationPath = userFolder + "/" + username;
    }

    if (existingStage.empty())
    {
        // Create the USD model in Omniverse
        const std::string stageUrl = createOmniverseModel(destinationPath, doLiveEdit, stageExtension);
        if (stageUrl.empty())
        {
            OMNI_LOG_FATAL("Failure to create model in Omniverse %s", stageUrl.c_str());
            exit(1);
        }

        existingStage = stageUrl;

        // Print the username for the server
        OMNI_LOG_INFO("Connected username: %s", omni::connect::core::getUser(stageUrl).c_str());

        // Create physics scene
        createPhysicsScene();

        // Create box geometry in the model
        boxMesh = createBox();

        // Create dynamic cube
        createDynamicCube(100.0);

        // Create quad - static tri mesh collision so that the box collides with it
        createQuad(500.0);

        // Create cylinders - This demonstrate how the display name and valid name work.
        createCylinders(20);

        // Create lights in the scene
        createDistantLight();
        createDomeLight("./Materials/kloofendal_48d_partly_cloudy.hdr");

        // Create a camera
        createCamera();

        // Upload a material and textures to the Omniverse server
        uploadReferences(destinationPath);

        // Add a material to the box
        createAndBindMaterial(
            boxMesh.GetPrim(),
            "Fieldstone",
            GfVec3f(0.4, 0.3, 0.2),
            "./Materials/Fieldstone/Fieldstone_BaseColor.png",
            "./Materials/Fieldstone/Fieldstone_N.png",
            "./Materials/Fieldstone/Fieldstone_ORM.png"
        );

        // Commit the changes to the USD
        saveStage(gStage, "Add material to the box");

        // Add a reference and payload, modify OmniPBR parameter
        createReferenceAndPayload();

        // Create an empty folder, just as an example
        createEmptyFolder(destinationPath + "/EmptyFolder");

        // Create an animated, skinned skeletal mesh off to the side
        createSkelMesh(gStage, GfVec3d(0, 0, -700));
        saveStage(gStage, "Add skel mesh to stage");

        // Add a final comment
        saveStage(gStage, finalCheckpointComment.c_str());
    }
    else
    {
        // Find a UsdGeomMesh in the existing stage
        boxMesh = findGeomMesh(existingStage);
    }

    // Do a live edit session moving the box around, changing a material
    if (doLiveEdit && boxMesh && existingStage.find(".live") != std::string::npos)
    {
        liveEdit(boxMesh, existingStage);
    }

    // All done, shut down our connection to Omniverse
    shutdownOmniverse();

    return 0;
}
