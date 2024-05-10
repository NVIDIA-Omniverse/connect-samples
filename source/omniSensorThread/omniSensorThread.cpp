// SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//

/*###############################################################################
#
# The Omniverse Sensor Thread is a command line program that continously pushes updates from an external
# source into an existing USD on the Nucleus Server. This is to demonstrate a simulated sensor sync
# path with a model in USD. This app simulates one sensor connected to a mesh in an existing USD created
# by Omniverse Simple Sensor.
#    * Two arguments,
#       1. The path to where to place the USD stage
#           * Acceptable forms:
#               * omniverse://localhost/Users/test
#               * C:\USD
#            * A relative path based on the CWD of the program (helloworld.usda)
#       2. The thread number
#           * Acceptable forms:
#              * 1
#              * 2, etc.
#    * Initialize Omniverse
#        * Set the Omniverse Client log callback (using a lambda)
#        * Set the Omniverse Client log level
#        * Initialize the Omniverse Client library
#        * Register a connection status callback (using a lambda)
#   * Attach to an existing USD stage
#   * Associate sensors to a mesh in the stage
#    * Set the USD stage URL as live
#    * Start a thread that loops, taking simulated sensor input from a randomnized seed
#        * Update the color characteristic of a portion of the USD stage
#    * Destroy the stage object
#    * Shutdown the Omniverse Client library
#
# eg. omniSensorThread.exe omniverse://localhost/Users/test  4
#
###############################################################################*/

#include <OmniClient.h>

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

PXR_NAMESPACE_USING_DIRECTIVE

// Globals for Omniverse Connection and base Stage
static UsdStageRefPtr gStage;

// Global for making the logging reasonable
static std::mutex gLogMutex;

// Global timer period
using namespace std::chrono_literals;
auto gTimerPeriod = 300ms;

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
static std::string openOmniverseModel(const std::string& destinationPath)
{
    std::string stageUrl = destinationPath;

    // Open the live stage
    std::cout << "    Opening the stage : " << stageUrl.c_str() << std::endl;
    gStage = pxr::UsdStage::Open(stageUrl);
    if (!gStage)
    {
        std::cout << "    Failure to open model in Omniverse: " << stageUrl.c_str() << std::endl;
        return std::string();
    }

    std::cout << "       Success in opening the stage" << std::endl;
    return stageUrl;
}

// Find the mesh we will change the color of in the current stage
UsdGeomMesh attachToZoneGeometry(int zone)
{
    std::string path = "/World/box_" + std::to_string(zone);
    std::cout << "    Opening prim at path: " << path << std::endl;
    UsdPrim prim = gStage->GetPrimAtPath(SdfPath(path));
    UsdGeomMesh meshPrim = pxr::UsdGeomMesh(prim);
    if (!meshPrim)
    {
        std::cout << "    Failure opening prim" << std::endl;
    }

    return meshPrim;
}

// This class contains a doWork method that's use as a thread's function
//    and members that allow for synchronization between the the file update
//  callbacks and a main thread that takes keyboard input
// It busy-loops using `omniClientLiveProcess` to recv updates from the server
//  to the USD stage.  If there are updates it will export the USDA file
//  within a second of receiving them.
class DataStageWriterWorker
{
public:

    DataStageWriterWorker() : stopped(false), variance(1.0f), step(0), runLimit(-1){};

    void doWork()
    {
        while (!stopped)
        {
            using namespace std::chrono_literals;

            // Setting a frequency of 300ms as a starting point for updates
            std::this_thread::sleep_for(gTimerPeriod);

            omniClientLiveProcess();

            // Update the color this zone in the model
            {
                // Make a color change for the cube
                UsdAttribute displayColorAttr = mesh.GetDisplayColorAttr();
                VtVec3fArray valueArray;
                GfVec3f rgbFace(0.463f * variance, 0.725f * variance, 0.0f);
                valueArray.push_back(rgbFace);

                // Use the mutex lock since we are making a change to the same layer from multiple threads
                // (if this were actually an MT thread program, in this case it is not)
                {
                    std::unique_lock<std::mutex> lk(gLogMutex);
                    displayColorAttr.Set(valueArray);
                }
                omniClientLiveProcess();
            }

            // Update the value of the variance - simulates the change in sensor reading
            step++;
            if (step >= 360)
            {
                step = 0;
            }
            variance = cos((double)step);
        }
    }
    std::atomic<bool> stopped;
    pxr::UsdStageRefPtr stage;
    int zone;
    UsdGeomMesh mesh;
    float variance;
    int step;
    int runLimit;
};

// The program expects three arguments, output USD path, processes and timeout in seconds
int main(int argc, char* argv[])
{
    if (argc != 4)
    {
        std::cout << "Please provide a path where to keep the USD model and thread number." << std::endl;
        std::cout << "   Arguments:" << std::endl;
        std::cout << "       1. Path to USD model (omniverse://localhost/Users/test)" << std::endl;
        std::cout << "       2. Number of boxes / processes" << std::endl;
        std::cout << "       3. Timeout in seconds (-1 for infinity)" << std::endl;
        std::cout << "Example - omniSensorThread.exe omniverse://localhost/Users/test 4 25" << std::endl;

        return -1;
    }

    std::cout << "Omniverse Sensor Thread: " << argv[1] << " " << argv[2] << std::endl;

    // Create the final model string URL
    std::string stageUrl(argv[1]);
    std::string baseUrl(argv[1]);

    // Which sensor are we attaching?
    int threadNumber = std::strtol(argv[2], nullptr, 10);

    // How long to run for?
    int timeout = -1;
    timeout = std::strtol(argv[3], nullptr, 10);

    stageUrl += "/SimpleSensorExample.live";

    // Initialize Omniverse via the Omni Client Lib
    startOmniverse();

    // Create the model in Omniverse
    std::string newStageUrl = openOmniverseModel(stageUrl);
    if (newStageUrl.length() == 0)
    {
        exit(1);
    }

    // Create the worker thread object
    DataStageWriterWorker stageWriter;
    stageWriter.zone = threadNumber;

    // Add zones of data to the model
    std::cout << "    Attach to the zone geometry" << std::endl;
    stageWriter.mesh = attachToZoneGeometry(threadNumber);
    stageWriter.stage = gStage;
    stageWriter.runLimit = timeout;

    // Start Live Edit with Omni Client Library
    omniClientLiveProcess();

    // Create a running thread
    auto duration = std::chrono::duration<double>(gTimerPeriod);
    std::cout << "    Worker thread started with timer period of " << duration.count() << " seconds for " << timeout << " seconds" << std::endl;

    std::thread workerThread(&DataStageWriterWorker::doWork, &stageWriter);

    std::time_t startTime = std::time(0);
    int elapsedTime = 0;
    while (timeout == -1 || elapsedTime < timeout)
    {
        // Add a slight pause so that the main thread is not operating at a high rate
        // Right now, checks the time every 5 seconds. Increase this if the timeout is very short.
        std::chrono::seconds pause(5);
        std::this_thread::sleep_for(pause);

        std::time_t newTime = std::time(0);
        elapsedTime = (newTime - startTime);
    }

    // Stop the threads
    stageWriter.stopped = true;

    // Wait for the threads to go away
    workerThread.join();

    shutdownOmniverse();
}
