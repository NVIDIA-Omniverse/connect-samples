// SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//

/*###############################################################################
#
# This liveSession sample demonstrates how to connect to a live session using
# the non-destructive live workflow.  A .live layer is used in the stage's session
# layer to contain the changes.  An Omniverse channel is used to broadcast users
# and merge notifications to all clients, and a session config (TOML) file is used
# to determine the "owner" of the session.
#
# * Initialize the Omniverse Resolver Plugin
# * Display existing live sessions for a stage
# * Connect to a live session
# * Make xform changes to a mesh prim in the .live layer
# * Rename a prim that exists in the .live layer
# * Display the owner of the live session
# * Display the current connected users/peers in the session
# * Emit a GetUsers message to the session channel
# * Display the contents of the session config
# * Merge the changes from the .live session back to the root stage
# * Respond (by exiting) when another user merges session changes back to the root stage
#
###############################################################################*/

#include <omni/connect/core/Core.h>
#include <omni/connect/core/LayerAlgo.h>
#include <omni/connect/core/LiveSession.h>
#include <omni/connect/core/LiveSessionChannel.h>
#include <omni/connect/core/LiveSessionConfig.h>
#include <omni/connect/core/LiveSessionInfo.h>
#include <omni/connect/core/PrimAlgo.h>
#include <omni/connect/core/StageAlgo.h>
#include <omni/connect/core/XformAlgo.h>

#include <carb/extras/Path.h>
#include <carb/filesystem/IFileSystem.h>

#include <cassert>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <conio.h>
#include <filesystem>
#include <windows.h>
namespace fs = std::filesystem;
#else
#include <experimental/filesystem>
#include <limits.h>
#include <unistd.h> //readlink
namespace fs = std::experimental::filesystem;
#endif

#include "OmniClient.h"
#include "OmniUsdResolver.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/primvar.h"

#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usdGeom/xform.h>

// Initialize the Omniverse application
OMNI_APP_GLOBALS("LiveSessionSample", "Omniverse LiveSession Connector");

PXR_NAMESPACE_USING_DIRECTIVE

// Globals for Omniverse Connection and base Stage
static bool gStageMerged = false;

// Shut down Omniverse connection
static void shutdownOmniverse()
{
    // Calling this prior to shutdown ensures that all pending live updates complete.
    omniClientLiveWaitForPendingUpdates();
}

// Get the Absolute path of the current executable
static fs::path getExePath()
{
    // This function is called VERY early so make sure the Carbonite framework is initialized.
    if (!carb::getFramework())
    {
        carb::acquireFrameworkAndRegisterBuiltins();
    }

    auto filesystem = carb::getCachedInterface<carb::filesystem::IFileSystem>();
    if (filesystem)
    {
        const char* exeDir = filesystem->getExecutableDirectoryPath();
        return exeDir;
    }
    else
    {
        OMNI_LOG_ERROR("Couldn't get the executable directory from Carbonite, returning an empty executable path");
        return "";
    }
}

// Startup Omniverse
static bool startOmniverse(bool verbose)
{
    // This is not strictly required for this sample because the sample copies all of the USD plugin
    // files to the correct place relative to the executable and current working directory.  This is
    // an instructional bit for apps that may not be able to do this.
    // Also note that part of the OMNICONNECTCORE_INIT() is initializing the USD Resolver plugin, so
    // this bit is merely instructional.

    // Find absolute path of the resolver plugins `resources` folder
    std::string pluginResourcesFolder = getExePath().string() + "/usd/omniverse/resources";
    PlugRegistry::GetInstance().RegisterPlugins(pluginResourcesFolder);
    std::string PluginName = "OmniUsdResolver";
    if (TfType::FindByName(PluginName).IsUnknown())
    {
        OMNI_LOG_FATAL("Could not find the Omniverse USD Resolver plugin");
        return false;
    }

    // Startup the core Omniverse frameworks required for Connectors
    // NOTE: this funciton will also initialize the Omniverse USD Resolver plugin
    OMNICONNECTCORE_INIT();
    if (!omni::connect::core::initialized())
    {
        return false;
    }

    auto log = omniGetLogWithoutAcquire();
    log->setLevel(verbose ? omni::log::Level::eVerbose : omni::log::Level::eInfo);

    // Handle connection errors. Note Connect SDK already provides logging for status changes,
    // so we only need to add any extra status handling.
    omniClientRegisterConnectionStatusCallback(
        nullptr,
        [](void* /* userData */, const char* /* url */, OmniClientConnectionStatus status) noexcept
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
                    OMNI_LOG_FATAL("There was a problem connecting to Nucleus (OmniClientConnectionStatus: %d). Exiting.", status);
                    // We shouldn't just exit here - we should clean up a bit, but we're going to do it anyway
                    _Exit(1);
                }
            }
        }
    );

    return true;
}

// Opens an existing stage and finds the first UsdGeomMesh
// If stageUri is empty, it will skip opening the stage and find a mesh in stage
static UsdGeomMesh findGeomMesh(UsdStagePtr stage)
{
    if (!stage)
    {
        OMNI_LOG_ERROR("No existing stage");
        return UsdGeomMesh();
    }

    // Traverse the stage and return the first UsdGeomMesh we find
    auto range = stage->Traverse();
    for (const auto& node : range)
    {
        if (node.IsA<UsdGeomMesh>())
        {
            OMNI_LOG_INFO("Found UsdGeomMesh: %s", node.GetPath().GetAsString().c_str());
            return UsdGeomMesh(node);
        }
    }

    // No UsdGeomMesh found in stage (what kind of stage is this anyway!?)
    OMNI_LOG_ERROR("No UsdGeomMesh found in stage");
    return UsdGeomMesh();
}

void findOrCreateSession(omni::connect::core::LiveSession* liveSession)
{
    UsdStageRefPtr liveStage;
    omni::connect::core::LiveSessionInfo::SessionNames sessionList = liveSession->getInfo()->getSessionNames();
    OMNI_LOG_INFO("Select or create a Live Session:");
    for (size_t i = 0; i < sessionList.size(); i++)
    {
        OMNI_LOG_INFO(" [%d] %s", (int)i, sessionList[i].c_str());
    }
    OMNI_LOG_INFO(" [n] Create a new session");
    OMNI_LOG_INFO(" [q] Quit");
    OMNI_LOG_INFO("Select a live session to join:");

    char selection;
    std::cin >> selection;

    // If the user picked a session, join it by name
    size_t selectionIdx = size_t(selection) - 0x30;
    std::string sessionName;
    if (std::isdigit(selection) && selectionIdx < sessionList.size())
    {
        sessionName = sessionList[selectionIdx];
    }
    else if ('n' == selection)
    {
        // Get a new session name
        OMNI_LOG_INFO("Enter the new session name: ");
        std::cin >> sessionName;
    }
    else
    {
        OMNI_LOG_INFO("Exiting");
        exit(0);
    }

    // Join the session and change the edit target to the new .live sublayer
    SdfLayerHandle liveLayer = liveSession->join(sessionName);
    if (!liveLayer)
    {
        OMNI_LOG_ERROR("Failed to join live session: %s", sessionName.c_str());
        exit(1);
    }
    else
    {
        OMNI_LOG_INFO("Successfully joined session: %s", sessionName.c_str());
    }
}

bool endAndMergeSession(omni::connect::core::LiveSession* liveSession)
{
    // Send a MERGE_STARTED channel message
    liveSession->getChannel()->sendMessage(omni::connect::core::LiveSessionChannel::MessageType::eMergeStarted);

    // Check if there are any prims defined in the stage's root layer.
    // Merging to a new layer if this is the case could result in no visible changes due to the
    // root layer taking priority
    bool rootHasSpecs = liveSession->hasPrimSpecsInRootLayer();

    std::string mergeOption("_");
    while (mergeOption != "n" && mergeOption != "r" && mergeOption != "c")
    {
        if (rootHasSpecs)
        {
            OMNI_LOG_WARN(
                "There are PrimSpecs defined in the root layer.  Changes from the live session could be hidden if they are merged to a new layer."
            );
        }
        OMNI_LOG_INFO("Merge to new layer [n], root layer [r], or cancel [c]: ");

        std::cin >> mergeOption;
    }
    if (mergeOption == "n")
    {
        // Inject a new layer in the same folder as the root with the session name into the root stage (rootStageName_sessionName_NN.usd)
        // This will also reset the edit target back to the stage's root layer
        liveSession->mergeToNewLayer();
    }
    else if (mergeOption == "r")
    {
        // Merge the live deltas to the root layer
        // This will also reset the edit target back to the stage's root layer
        liveSession->mergeToRoot();
    }
    else // the merge was canceled
    {
        return false;
    }

    return true;
}


// Perform a live edit on the box
static void liveEdit(UsdStagePtr stage, UsdGeomMesh& geomMesh, omni::connect::core::LiveSession* liveSession)
{
    double angle = 0;
    const char* optionsStr =
        "Enter an option:\n"
        " [t] transform the mesh\n"
        " [r] rename a prim\n"
        " [o] list session owner/admin\n"
        " [u] list session users\n"
        " [g] emit a GetUsers message (note there will be no response unless another app is connected to the same session)\n"
        " [c] log contents of the session config file\n"
        " [m] merge changes and end the session\n"
        " [q] quit.";

    // Process any updates that may have happened to the stage from another client
    omniClientLiveProcess();
    OMNI_LOG_INFO("Begin Live Edit on %s", geomMesh.GetPath().GetText());
    OMNI_LOG_INFO("%s", optionsStr);
    bool wait = true;
    while (wait)
    {
#ifdef _WIN32
        char nextCommand = _getch();
#else
        char nextCommand = getchar();
#endif
        // Check if the live session was merged by another client and exit if so
        // A more sophisticated client should reload the stage without the live session layer
        if (gStageMerged)
        {
            return;
        }

        // Process any updates that may have happened to the stage from another client
        omniClientLiveProcess();

        switch (nextCommand)
        {
            case 't':
            {
                // if the prim is no longer valid - maybe it was removed...
                if (!geomMesh.GetPrim().IsValid() || !geomMesh.GetPrim().IsActive())
                {

                    OMNI_LOG_WARN("Prim no longer valid and active, finding a new one to move...");
                    geomMesh = findGeomMesh(stage);
                    if (geomMesh)
                    {
                        OMNI_LOG_INFO("Found new mesh to move: %s", geomMesh.GetPrim().GetPath().GetAsString().c_str());
                    }
                }

                if (!geomMesh.GetPrim().IsValid() || !geomMesh.GetPrim().IsActive())
                {
                    break;
                }

                // Increase the angle
                angle += 15;
                if (angle >= 360)
                {
                    angle = 0;
                }

                double radians = angle * 3.1415926 / 180.0;
                double x = sin(radians) * 10;
                double y = cos(radians) * 10;

                GfVec3d position(0);
                GfVec3d pivot(0);
                GfVec3f rotXYZ(0);
                omni::connect::core::RotationOrder rotationOrder;
                GfVec3f scale(1);

                UsdPrim meshPrim = geomMesh.GetPrim();
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
            case 'r':
            {
                std::string primToRename;
                UsdPrim prim;
                OMNI_LOG_INFO("Enter complete prim path to rename: ");
                std::cin >> primToRename;

                // Get the prim
                prim = stage->GetPrimAtPath(SdfPath(primToRename));
                if (!prim)
                {
                    OMNI_LOG_WARN("Could not find prim: %s", primToRename.c_str());
                }
                else
                {
                    std::string newName;
                    OMNI_LOG_INFO("Enter new prim name (not entire path):");
                    std::cin >> newName;

                    TfTokenVector nameVector = omni::connect::core::getValidChildNames(prim, { newName });
                    if (omni::connect::core::renamePrim(prim, nameVector[0]))
                    {
                        // Commit the change to USD
                        omniClientLiveProcess();
                        OMNI_LOG_INFO("%s renamed to: %s", primToRename.c_str(), nameVector[0].GetString().c_str());
                    }
                    else
                    {
                        OMNI_LOG_ERROR("%s rename failed.", primToRename.c_str());
                    }
                }
                break;
            }
            case 'o':
            {
                std::string configUri = liveSession->getInfo()->getConfigUri();
                omni::connect::core::LiveSessionConfigMap configMap = omni::connect::core::getLiveSessionConfig(configUri.c_str());
                std::string sessionAdmin = configMap[omni::connect::core::LiveSessionConfig::eAdmin];
                OMNI_LOG_INFO("Session Admin: %s", sessionAdmin.c_str());
                break;
            }
            case 'u':
            {
                omni::connect::core::LiveSessionChannel::Users users = liveSession->getChannel()->getUsers(/* includeSelf */ false);
                OMNI_LOG_INFO("Listing Session users:");
                if (users.empty())
                {
                    OMNI_LOG_INFO(" - No other users in session");
                }
                for (const auto& user : users)
                {
                    OMNI_LOG_INFO(" - %s [%s]", user.name.c_str(), user.app.c_str());
                }
                break;
            }
            case 'g':
            {
                // Send a GET_USERS channel message, all the other connected clients will respond with a "HELLO"
                // The callbacks could be used to fill in a UI list of connected users when browsing sessions.
                // This is done _BEFORE_ joining a session, but it's convenient to just put it here as an example
                OMNI_LOG_INFO("Blasting GET_USERS message to channel");
                liveSession->getChannel()->sendMessage(omni::connect::core::LiveSessionChannel::MessageType::eGetUsers);
                break;
            }
            case 'c':
            {
                OMNI_LOG_INFO("Retrieving session config file: ");
                std::string configUri = liveSession->getInfo()->getConfigUri();
                omni::connect::core::LiveSessionConfigMap configMap = omni::connect::core::getLiveSessionConfig(configUri.c_str());

                for (auto k : { omni::connect::core::LiveSessionConfig::eVersion,
                                omni::connect::core::LiveSessionConfig::eAdmin,
                                omni::connect::core::LiveSessionConfig::eStageUri,
                                omni::connect::core::LiveSessionConfig::eDescription,
                                omni::connect::core::LiveSessionConfig::eMode,
                                omni::connect::core::LiveSessionConfig::eName })
                {
                    // Quick closure to translate the session config key enum to a string
                    auto configToString = [](omni::connect::core::LiveSessionConfig config)
                    {
                        switch (config)
                        {
                            case omni::connect::core::LiveSessionConfig::eVersion:
                                return "Version";
                            case omni::connect::core::LiveSessionConfig::eAdmin:
                                return "Admin";
                            case omni::connect::core::LiveSessionConfig::eStageUri:
                                return "StageUri";
                            case omni::connect::core::LiveSessionConfig::eDescription:
                                return "Description";
                            case omni::connect::core::LiveSessionConfig::eMode:
                                return "Mode";
                            case omni::connect::core::LiveSessionConfig::eName:
                                return "Name";
                            default:
                                return "Invalid";
                        }
                    };

                    std::string value = configMap[k];
                    if (!value.empty())
                    {
                        OMNI_LOG_INFO(" %s = \"%s\"", configToString(k), value.c_str());
                    }
                }
                break;
            }
            case 'm':
            {
                OMNI_LOG_INFO("Merging session changes to root layer, Live Session complete");
                if (endAndMergeSession(liveSession))
                {
                    wait = false;
                }
                break;
            }
            // escape or 'q'
            case 27:
            case 'q':
            {
                wait = false;
                OMNI_LOG_INFO("Live Edit complete");
                break;
            }
            default:
                OMNI_LOG_INFO("%s", optionsStr);
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
        "    -e, --existing path_to_stage  Open an existing stage and perform live transform edits (full omniverse URL)\n"
        "    -v, --verbose                 Show the verbose Omniverse logging\n"
        "\n\nExamples:\n"
        "\n * live edit a stage on the localhost server at /Projects/LiveEdit/livestage.usd\n"
        "    > samples -e omniverse://localhost/Projects/LiveEdit/livestage.usd\n"
    );
}

// This class contains a run() method that's run in a thread to
// tick any message channels (it will flush any messages received
// from the Omniverse Client Library
class AppUpdate
{
public:

    AppUpdate(int updatePeriodMs) : mUpdatePeriodMs(updatePeriodMs), mStopped(false)
    {
    }

    void run()
    {
        while (!mStopped)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(mUpdatePeriodMs));
            for (auto& channel : mChannels)
            {
                omni::connect::core::LiveSessionChannel::Messages messages = channel->processMessages();
                for (const auto& msg : messages)
                {
                    OMNI_LOG_INFO(
                        "Channel Message: %s %s - %s",
                        omni::connect::core::LiveSessionChannel::getMessageTypeName(msg.type),
                        msg.user.name.c_str(),
                        msg.user.app.c_str()
                    );
                    if (msg.type == omni::connect::core::LiveSessionChannel::MessageType::eMergeStarted ||
                        msg.type == omni::connect::core::LiveSessionChannel::MessageType::eMergeFinished)
                    {
                        OMNI_LOG_WARN("Exiting since a merge is happening in another client");
                        gStageMerged = true;
                    }
                }
            }
        }
    }

    int mUpdatePeriodMs;
    bool mStopped;
    std::vector<std::shared_ptr<omni::connect::core::LiveSessionChannel>> mChannels;
};


// Main Application
int main(int argc, char* argv[])
{
    bool doLiveEdit = false;
    std::string existingStageUri;
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
                OMNI_LOG_FATAL("Missing an Omniverse URL to the stage to edit.");
                printCmdLineArgHelp();
                return -1;
            }
            existingStageUri = std::string(argv[++x]);

            if (!omni::connect::core::isOmniUri(existingStageUri))
            {
                OMNI_LOG_ERROR(
                    "This is not an Omniverse Nucleus URL: %s\n"
                    "Correct Omniverse URL format is: omniverse://server_name/Path/To/Example/Folder\n",
                    existingStageUri.c_str()
                );
                return -1;
            }
            else if (!omni::connect::core::doesUriExist(existingStageUri))
            {
                OMNI_LOG_ERROR("Provided stage URL does not exist - %s", existingStageUri.c_str());
                return -1;
            }
        }
        else
        {
            OMNI_LOG_ERROR("Unrecognized option: %s", argv[x]);
        }
    }

    if (existingStageUri.length() == 0)
    {
        OMNI_LOG_ERROR("An existing stage must be supplied with the -e argument: ");
        return -1;
    }

    // Open the stage
    UsdStageRefPtr stage = UsdStage::Open(existingStageUri);
    if (!stage)
    {
        OMNI_LOG_FATAL("Failure to open stage in Omniverse: %s", existingStageUri.c_str());
        exit(1);
    }

    // Find a UsdGeomMesh in the existing stage
    boxMesh = findGeomMesh(stage);

    // Create a LiveSession instance - we can browse the available sessions and join/create one.
    std::shared_ptr<omni::connect::core::LiveSession> liveSession = omni::connect::core::LiveSession::create(stage);
    if (!liveSession)
    {
        OMNI_LOG_ERROR("Failure to create a live session for stage: %s", existingStageUri.c_str());
        exit(1);
    }
    findOrCreateSession(liveSession.get());

    // Create a thread that "ticks" every 16ms
    // The only thing it does is "Update" the Omniverse Message Channels to
    // flush out any messages in the queue that were received.
    AppUpdate appUpdate(16);
    appUpdate.mChannels.push_back(liveSession->getChannel());
    std::thread channelUpdateThread = std::thread(&AppUpdate::run, &appUpdate);

    // Do a live edit session moving the box around, changing a material
    if (doLiveEdit)
    {
        liveEdit(stage, boxMesh, liveSession.get());
    }
    appUpdate.mStopped = true;
    channelUpdateThread.join();
    appUpdate.mChannels.clear(); // dereference the channels so they can be handled by the liveSession pointer reset

    // This will leave and close the session if we didn't merge
    liveSession.reset();

    // Close the stage as well for good measure
    stage.Reset();

    // All done, shut down our connection to Omniverse
    shutdownOmniverse();

    return 0;
}
