// SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//
/*###############################################################################
#
# The Omniverse Simple Sensor is a command line program that continously pushes updates from an external
# source into an existing USD on the Nucleus Server. This is to demonstrate a simulated sensor sync
# path with a model in USD.
#    * Two arguments,
#       1. The path to where to place the USD stage
#           * Acceptable forms:
#               * omniverse://localhost/Users/test
#               * C:\USD
#            * A relative path based on the CWD of the program (helloworld.usda)
#       2. The number of threads pushing simulated sensor data
#           * Acceptable forms:
#              * 1
#              * 2, etc.
#   * Create a USD stage
#    * Destroy the stage object
#    * Shutdown the Omniverse Client library
#
# eg. omniSimpleSensor.exe omniverse://localhost/Users/test  4
#
###############################################################################*/

#include <omni/connect/core/Core.h>
#include <omni/connect/core/LightAlgo.h>
#include <omni/connect/core/LightCompatibility.h>
#include <omni/connect/core/MeshAlgo.h>
#include <omni/connect/core/PrimAlgo.h>
#include <omni/connect/core/XformAlgo.h>

#include <OmniClient.h>

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdLux/domeLight.h>
#if PXR_VERSION >= 2208
#include <pxr/usd/usdGeom/primvarsAPI.h>
#endif

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <thread>

PXR_NAMESPACE_USING_DIRECTIVE

// Globals for Omniverse Connection and base Stage
static UsdStageRefPtr gStage;

// Multiplatform array size
#define HW_ARRAY_COUNT(array) (sizeof(array) / sizeof(array[0]))

// Startup Omniverse
static bool startOmniverse()
{
    // Register a function to be called whenever the library wants to print something to a log
    omniClientSetLogCallback(
        [](char const* /* threadName */, char const* /* component */, OmniClientLogLevel level, char const* message) noexcept
        {
            std::cout << "[" << omniClientGetLogLevelString(level) << "] " << message << std::endl;
        }
    );

    // The default log level is "Info", set it to "Debug" to see all messages
    omniClientSetLogLevel(eOmniClientLogLevel_Warning);

    // Initialize the library and pass it the version constant defined in OmniClient.h
    // This allows the library to verify it was built with a compatible version. It will
    // return false if there is a version mismatch.
    if (!omniClientInitialize(kOmniClientVersion))
    {
        return false;
    }

    omniClientRegisterConnectionStatusCallback(
        nullptr,
        [](void* /* userData */, const char* url, OmniClientConnectionStatus status) noexcept
        {
            std::cout << "Connection Status: " << omniClientGetConnectionStatusString(status) << " [" << url << "]" << std::endl;
            if (status == eOmniClientConnectionStatus_ConnectError)
            {
                // We shouldn't just exit here - we should clean up a bit, but we're going to do it anyway
                std::cout << "[ERROR] Failed connection, exiting." << std::endl;
                exit(-1);
            }
        }
    );

    return true;
}

// Shut down Omniverse connection
static void shutdownOmniverse()
{
    // Calling this prior to shutdown ensures that all pending live updates complete.
    omniClientLiveWaitForPendingUpdates();

    // The stage is a sophisticated object that needs to be destroyed properly.
    // Since gStage is a smart pointer we can just reset it
    gStage.Reset();

    // This will prevent "Core::unregister callback called after shutdown"
    omniClientSetLogCallback(nullptr);

    omniClientShutdown();
}


// Create a new connection for this model in Omniverse, returns the created stage URL
static std::string createOmniverseModel(const std::string& destinationPath)
{
    std::string stageUrl = destinationPath;

    // Open the old version of this file on Omniverse or create it
    std::cout << "    Creating or modifying " << stageUrl << std::endl;

    // The default prim
    pxr::SdfPath defaultPrimPath = pxr::SdfPath("/World");

    // We could just use UsdStage::Open() success, but it emits a Runtime Error if the stage doesn't exist
    if (!omni::connect::core::doesUriExist(stageUrl.c_str()))
    {
        // Create this file in Omniverse cleanly
        gStage = UsdStage::CreateNew(stageUrl);
        if (!gStage)
        {
            std::cout << "    Failure to create stage in Omniverse: ";
            std::cout << stageUrl.c_str();
            std::cout << std::endl;
            return std::string();
        }

        std::cout << "    New stage created: " << stageUrl << std::endl;
    }
    else
    {
        gStage = UsdStage::Open(stageUrl);
    }

    if (gStage->GetPrimAtPath(defaultPrimPath))
    {
        // Remove the default prim and everything under it, start clean
        gStage->RemovePrim(defaultPrimPath);
    }

    pxr::UsdGeomXform rootXform = omni::connect::core::defineXform(gStage, defaultPrimPath);
    pxr::UsdPrim rootPrim = gStage->GetPrimAtPath(defaultPrimPath);
    gStage->SetDefaultPrim(rootPrim);

    // Always a good idea to declare your up-ness
    UsdGeomSetStageUpAxis(gStage, UsdGeomTokens->y);

    return stageUrl;
}

// Create a light source in the scene.
static void createDomeLight(const std::string& texturePath)
{
    // Construct /Root/Light path
    SdfPath lightPath = SdfPath("/World/Domelight");

    // Define dome light
    UsdLuxDomeLight newLight = omni::connect::core::defineDomeLight(gStage, lightPath, 1000.0f, texturePath.c_str(), UsdLuxTokens->latlong);

    // Set rotation on domelight
    UsdGeomXformable xForm = newLight;
    UsdGeomXformOp rotateOp;
    GfVec3d rotXYZ(270, 0, 0);
    rotateOp = xForm.AddXformOp(UsdGeomXformOp::TypeRotateXYZ, UsdGeomXformOp::Precision::PrecisionDouble);
    rotateOp.Set(rotXYZ);

    // Commit the changes to the USD
    gStage->Save();
}

struct Info
{
    UsdGeomMesh mesh;
    UsdStageRefPtr stage;
};


// Box mesh data
double gVal = 50.0;
int gBoxVertexIndices[] = { 0,  1,  2,  1,  3,  2,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
                            12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23 };
double gBoxNormals[][3] = { { 0, 0, -1 }, { 0, 0, -1 }, { 0, 0, -1 }, { 0, 0, -1 }, { 0, 0, 1 },  { 0, 0, 1 },  { 0, 0, 1 },  { 0, 0, 1 },
                            { 0, -1, 0 }, { 0, -1, 0 }, { 0, -1, 0 }, { 0, -1, 0 }, { 1, 0, 0 },  { 1, 0, 0 },  { 1, 0, 0 },  { 1, 0, 0 },
                            { 0, 1, 0 },  { 0, 1, 0 },  { 0, 1, 0 },  { 0, 1, 0 },  { -1, 0, 0 }, { -1, 0, 0 }, { -1, 0, 0 }, { -1, 0, 0 } };
double gBoxPoints[][3] = { { gVal, -gVal, -gVal },  { -gVal, -gVal, -gVal }, { gVal, gVal, -gVal },  { -gVal, gVal, -gVal }, { gVal, gVal, gVal },
                           { -gVal, gVal, gVal },   { -gVal, -gVal, gVal },  { gVal, -gVal, gVal },  { gVal, -gVal, gVal },  { -gVal, -gVal, gVal },
                           { -gVal, -gVal, -gVal }, { gVal, -gVal, -gVal },  { gVal, gVal, gVal },   { gVal, -gVal, gVal },  { gVal, -gVal, -gVal },
                           { gVal, gVal, -gVal },   { -gVal, gVal, gVal },   { gVal, gVal, gVal },   { gVal, gVal, -gVal },  { -gVal, gVal, -gVal },
                           { -gVal, -gVal, gVal },  { -gVal, gVal, gVal },   { -gVal, gVal, -gVal }, { -gVal, -gVal, -gVal } };
float gBoxUV[][2] = { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 },
                      { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 } };


// Create the sections of geometry in the model
Info createZoneGeometry(int zoneNumber, int totalZones, std::string)
{
    // Create a simple box in USD with normals and UV information
    // Box name and path
    std::string boxName = "box_";
    boxName.append(std::to_string(zoneNumber));
    boxName = omni::connect::core::getValidPrimName(boxName);
    SdfPath boxPath = SdfPath::AbsoluteRootPath().AppendChild(TfToken("World")).AppendChild(TfToken(boxName));

    // Calculate the offset for the box based on the zone number
    int zoneSize = (int)floor(std::cbrt((double)totalZones));
    if (zoneSize < 1)
    {
        zoneSize = 1;
    }
    float xOffset = (zoneNumber % zoneSize) * 150;
    int yZone = zoneNumber % (zoneSize * zoneSize);
    float yOffset = int(yZone / zoneSize) * 150;
    float zOffset = int(floor(zoneNumber / (zoneSize * zoneSize))) * 150;

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
        points[i] = GfVec3f(gBoxPoints[i][0] + xOffset, gBoxPoints[i][1] + yOffset, gBoxPoints[i][2] + zOffset);
    }

    // normals
    int num_normals = HW_ARRAY_COUNT(gBoxNormals);
    VtArray<GfVec3f> normals;
    normals.resize(num_normals);
    for (int i = 0; i < num_normals; i++)
    {
        normals[i] = GfVec3f((float)gBoxNormals[i][0], (float)gBoxNormals[i][1], (float)gBoxNormals[i][2]);
    }

    // UV (st)
    int uv_count = HW_ARRAY_COUNT(gBoxUV);
    VtVec2fArray uvs;
    uvs.resize(uv_count);
    for (int i = 0; i < uv_count; ++i)
    {
        uvs[i].Set(gBoxUV[i]);
    }

    UsdGeomMesh mesh = omni::connect::core::definePolyMesh(
        gStage,
        boxPath,
        faceVertexCounts,
        faceVertexIndices,
        points,
        omni::connect::core::Vec3fPrimvarData(UsdGeomTokens->vertex, normals),
        omni::connect::core::Vec2fPrimvarData(UsdGeomTokens->vertex, uvs),
        omni::connect::core::Vec3fPrimvarData(UsdGeomTokens->constant, { { 0.463f, 0.725f, 0.0f } }) // displayColor
    );

    // Define the information to pass back
    Info returnInfo;
    returnInfo.mesh = mesh;
    returnInfo.stage = gStage;

    if (!mesh)
    {
        return returnInfo;
    }

    return returnInfo;
}

// The program expects three arguments, output USD path, processes and timeout in seconds
int main(int argc, char* argv[])
{
    if (argc != 4)
    {
        std::cout << "Please provide a path where to keep the USD model and thread count." << std::endl;
        std::cout << "   Arguments:" << std::endl;
        std::cout << "       1. Path to USD model (omniverse://localhost/Users/test)" << std::endl;
        std::cout << "       2. Number of boxes / processes" << std::endl;
        std::cout << "       3. Timeout in seconds (-1 for infinity)" << std::endl;
        std::cout << "Example - omniSimpleSensor.exe omniverse://localhost/Users/test 4 10" << std::endl;
        exit(1);
    }

    std::cout << "Omniverse Simple Sensor: " << argv[1] << " -> " << argv[2] << std::endl;

    // Create the final model string URL
    std::string stageUrl(argv[1]);
    std::string baseUrl(argv[1]);
    stageUrl += "/SimpleSensorExample.live";

    // How many replicated rooms do we need to generate and then match sensors to?
    int numberOfThreads = std::strtol(argv[2], nullptr, 10);

    // Initialize Omniverse via the Omni Client Lib
    startOmniverse();

    // Create the model in Omniverse
    const std::string newStageUrl = createOmniverseModel(stageUrl);
    if (!gStage)
    {
        std::cout << "    Failure to create stage.  Exiting." << std::endl;
        exit(1);
    }

    // Upload Dome Light texture to the Omniverse Server
    const std::string domeLightHdr("kloofendal_48d_partly_cloudy.hdr");
    {
        std::string uriPath = baseUrl + "/Materials/" + domeLightHdr;
        std::cout << "    Upload the dome light texture" << std::endl;
        omniClientWait(
            omniClientCopy(std::string("resources/Materials/" + domeLightHdr).c_str(), uriPath.c_str(), nullptr, nullptr, eOmniClientCopy_Overwrite)
        );
    }

    // Create a dome light to give it a nice sky
    std::cout << "    Create the dome light" << std::endl;
    createDomeLight("./Materials/" + domeLightHdr);

    // Initialize the worker threads structure that exports the USDA file
    std::cout << "    Create the zone geometry" << std::endl;
    for (int x = 0; x < numberOfThreads; x++)
    {
        // Add zones of data to the model
        Info returnInfo = createZoneGeometry(x, numberOfThreads, baseUrl);
    }

    gStage->Save();
    std::cout << "    All geometry created" << std::endl;

    shutdownOmniverse();

    exit(0);
}
