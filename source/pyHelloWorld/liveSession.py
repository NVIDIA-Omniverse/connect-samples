# SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: MIT
#

###############################################################################
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
# * Display the owner of the live session
# * Display the current connected users/peers in the session
# * Emit a GetUsers message to the session channel
# * Display the contents of the session config
# * Merge the changes from the .live session back to the root stage
# * Respond (by exiting) when another user merges session changes back to the root stage
#
###############################################################################

# Python built-in
import argparse
import inspect
import math
import os
import sys
import threading
import time

# Python 3.8 - can't use PATH any longer
if hasattr(os, "add_dll_directory"):
    scriptdir = os.path.dirname(os.path.realpath(__file__))
    dlldir = os.path.abspath(os.path.join(scriptdir, "../../_build/windows-x86_64/release"))
    os.add_dll_directory(dlldir)

# Internal imports
import get_char_util
import omni.asset_validator.core
import omni.client

# Omni imports
import omni.connect.core
import omni.log
import omni.usd_resolver

# USD imports
from pxr import Gf, Sdf, Tf, Usd, UsdGeom, UsdUtils

g_connection_status_subscription = None
g_stage_merged = False


def connectionStatusCallback(url, connectionStatus):
    if connectionStatus not in (
        omni.client.ConnectionStatus.CONNECTING,
        omni.client.ConnectionStatus.CONNECTED,
    ):
        omni.log.fatal("[ERROR] Failed connection, exiting.", channel="PyLiveSessionSample")
        shutdownOmniverse()


def startOmniverse(verbose: bool):
    if not omni.connect.core.startup():
        omni.log.fatal("Unable to initialize Omniverse client, exiting.", channel="PyLiveSessionSample")
        sys.exit(1)

    omni.log.get_log().level = omni.log.Level.VERBOSE if verbose else omni.log.Level.INFO

    global g_connection_status_subscription
    g_connection_status_subscription = omni.client.register_connection_status_callback(connectionStatusCallback)

    # Add this material path so that the Asset Validator can resolve paths to the core MDL files like OmniPBR.mdl
    scriptdir = os.path.dirname(os.path.realpath(__file__))
    core_mdl_path = f'file:{os.path.abspath(os.path.join(scriptdir, "../../_build/target-deps/omni_core_materials/Base"))}'
    omni.client.add_default_search_path(core_mdl_path)


def shutdownOmniverse():
    omni.client.live_wait_for_pending_updates()

    global g_connection_status_subscription
    g_connection_status_subscription = None


def findGeomMesh(stage, search_mesh_str=""):
    for node in stage.Traverse():
        if search_mesh_str:
            if node.IsA(UsdGeom.Xformable):
                node_path = str(node.GetPath())
                if search_mesh_str in node_path:
                    return UsdGeom.Xformable(node)
        else:
            if node.IsA(UsdGeom.Mesh):
                return UsdGeom.Xformable(node)

    return UsdGeom.Xformable()


def list_session_users(live_channel):
    include_self = False
    users = live_channel.getUsers(include_self)
    omni.log.info("Listing session users: ", channel="PyLiveSessionSample")
    if len(users) == 0:
        omni.log.info(" - No other users in session", channel="PyLiveSessionSample")
    for user in users:
        omni.log.info(f" - {user.name} [{user.app}]", channel="PyLiveSessionSample")


def join_session_channel():
    """Join the live session channel"""

    channel_file_url = g_live_session_info.getChannelUri()
    live_channel = omni.connect.core.LiveSessionChannel.create(channel_file_url)
    if not live_channel or not live_channel.isConnected():
        omni.log.error(f"Could not connect to live session message channel: {channel_file_url}")
    return live_channel


def find_or_create_session(live_session):
    """Find or Create Session
    This function displays the existing session and allows the user to create a new session

    It will look for this folder structure (expecting stageUrl to contain 'myfile'):
        <.live> / < my_usd_file.live> / <session_name> / root.live
    If it doesn't find any sessions for the current USD file then it will present the option
    to create a new session.
    """
    # get the folder contains the sessions
    session_names = live_session.getInfo().getSessionNames()

    # list the available sessions, allow the user to pick one
    omni.log.info("Select a live session to join: ", channel="PyLiveSessionSample")
    session_idx = 0
    for session_name in session_names:
        omni.log.info(f" [{session_idx}] {session_name}", channel="PyLiveSessionSample")
        session_idx += 1
    omni.log.info(f" [n] Create a new session", channel="PyLiveSessionSample")
    omni.log.info(f" [q] Quit", channel="PyLiveSessionSample")

    session_idx_selected = input("Select a live session to join: ")
    session_name = None

    # the user picked a session, find the root.live file
    if session_idx_selected.isnumeric() and int(session_idx_selected) < session_idx:
        session_name = session_names[int(session_idx_selected)]

    # the user wants a new session, get the new session name
    elif session_idx_selected == "n":
        session_name = input("Enter the new session name: ")
    else:
        omni.log.error(f"Invalid selection, exiting", channel="PyLiveSessionSample")
        return False

    # Join the session and change the edit target to the new .live sublayer
    live_layer = live_session.join(session_name)
    if not live_layer:
        omni.log.error(f"Failed to join live session: {session_name}, exiting", channel="PyLiveSessionSample")
        return False

    return True


def end_and_merge_session(live_session):
    """
    End and Merge Session - This function will check that it has ownership (from the TOML file), then merge live deltas to the root layer
    """

    # send a merge started message
    live_session.getChannel().sendMessage(omni.connect.core.LiveSessionChannel.MessageType.eMergeStarted)

    # Check if there are any prims defined in the stage's root layer.
    # Merging to a new layer if this is the case could result in no visible changes due to the
    # root layer taking priority
    has_root_layer_prim_specs = live_session.hasPrimSpecsInRootLayer()

    merge_option = "_"
    while merge_option != "n" and merge_option != "r" and merge_option != "c":
        if has_root_layer_prim_specs:
            omni.log.warn(
                "There are PrimSpecs defined in the root layer.  Changes from the live session could be hidden if they are merged to a new layer.",
                channel="PyLiveSessionSample",
            )
        merge_option = input("Merge to new layer [n], root layer [r], or cancel [c]: ")

    if merge_option == "n":
        # Inject a new layer in the same folder as the root with the session name into the root stage (rootStageName_sessionName_NN.usd)
        live_session.mergeToNewLayer()
    elif merge_option == "r":
        # Merge the live deltas to the root layer
        live_session.mergeToRoot()
    else:
        return False

    return True


def run_live_edit(stage, prim, live_session):
    global g_stage_merged
    angle = 0
    prim_path = ""
    if prim:
        prim_path = prim.GetPath()
    prompt_msg = inspect.cleandoc(
        """

        Enter an option:
         [t] transform the mesh
         [r] rename a prim
         [o] list session owner/admin
         [u] list session users
         [g] emit a GetUsers message (note there will be no response unless another app is connected to the same session)
         [c] log contents of the session config file
         [v] validate the current state of the session layer
         [m] merge changes and end the session
         [q] quit.
         """
    )
    omni.log.info(f"Begin Live Edit on {prim_path}", channel="PyLiveSessionSample")
    omni.log.info(f"{prompt_msg}", channel="PyLiveSessionSample")

    while True:
        option = get_char_util.getChar()

        if g_stage_merged:
            omni.log.info(f"Exiting since a merge has completed", channel="PyLiveSessionSample")
            option = b"q"

        omni.client.live_process()
        if option == b"t":
            # if the prim is no longer valid - maybe it was removed...
            if not prim.IsValid() or not prim.IsActive():
                omni.log.info("Prim no longer valid, finding a new one to move", channel="PyLiveSessionSample")
                geomMesh = findGeomMesh(stage)
                if geomMesh:
                    prim = geomMesh.GetPrim()
                    omni.log.info(f"Found new mesh to move: {prim.GetPath()}", channel="PyLiveSessionSample")

            if not prim.IsValid() or not prim.IsActive():
                break

            angle = (angle + 15) % 360
            radians = angle * 3.1415926 / 180.0
            x = math.sin(radians) * 10.0
            y = math.cos(radians) * 10.0

            # Get srt transform from prim
            translate, pivot, rot_xyz, rotationOrder, scale = omni.connect.core.getLocalTransformComponents(prim, time=Usd.TimeCode.Default())

            # Translate and rotate
            translate += Gf.Vec3d(x, 0.0, y)
            rot_xyz = Gf.Vec3f(rot_xyz[0], angle, rot_xyz[2])

            omni.log.info(
                f"Setting pos [{translate[0]:.2f}, {translate[1]:.2f}, {translate[2]:.2f}] and rot [{rot_xyz[0]:.2f}, {rot_xyz[1]:.2f}, {rot_xyz[2]:.2f}]",
                channel="PyLiveSessionSample",
            )

            # Set srt transform
            omni.connect.core.setLocalTransform(
                prim,
                translation=translate,
                pivot=pivot,
                rotation=rot_xyz,
                rotationOrder=rotationOrder,
                scale=scale,
                time=Usd.TimeCode.Default(),
            )

            # Send live updates to the server and process live updates received from the server
            omni.client.live_process()

        elif option == b"r":
            prim_to_rename = input("Enter complete prim path to rename: ")
            prim = stage.GetPrimAtPath(prim_to_rename)
            if not prim:
                omni.log.warn(f"Could not find prim: {prim_to_rename}", channel="PyLiveSessionSample")
            else:
                new_name = input("Enter new prim name (not entire path):")
                name_vector = omni.connect.core.getValidChildNames(prim, [new_name])
                if omni.connect.core.renamePrim(prim, name_vector[0]):
                    # Commit the change to USD
                    omni.client.live_process()
                    omni.log.info(f"{prim_to_rename} renamed to: {name_vector[0]}", channel="PyLiveSessionSample")
                else:
                    omni.log.info(f"{prim_to_rename} rename failed.", channel="PyLiveSessionSample")

        elif option == b"o":
            config_map = omni.connect.core.getLiveSessionConfig(live_session.getInfo().getConfigUri())
            session_owner = config_map[omni.connect.core.LiveSessionConfig.eAdmin]
            omni.log.info(f"Session Owner: {session_owner}", channel="PyLiveSessionSample")

        elif option == b"u":
            list_session_users(live_session.getChannel())

        elif option == b"g":
            omni.log.info("Blasting GET_USERS message to channel", channel="PyLiveSessionSample")
            # send a get_users message
            live_session.getChannel().sendMessage(omni.connect.core.LiveSessionChannel.MessageType.eGetUsers)

        elif option == b"c":
            omni.log.info("Retrieving session config file: ", channel="PyLiveSessionSample")
            config_map = omni.connect.core.getLiveSessionConfig(live_session.getInfo().getConfigUri())
            session_owner = config_map[omni.connect.core.LiveSessionConfig.eAdmin]
            for config in config_map.keys():
                omni.log.info(f"  {config}: {config_map[config]}", channel="PyLiveSessionSample")

        elif option == b"v":
            omni.log.info("Validating current state", channel="PyLiveSessionSample")
            results = omni.asset_validator.core.ValidationEngine().validate(stage)
            if not results.issues():
                omni.log.info("The stage is valid", channel="PyLiveSessionSample")
            for issue in results.issues(omni.asset_validator.core.IssuePredicates.IsFailure()):
                # str(issue) also works, but this is formatted for readability like it appears in Kit
                omni.log.error("--------------------------------------------------------------------------------", channel="PyLiveSessionSample")
                if issue.rule:
                    omni.log.error(f"Rule:  {issue.rule.__name__}", channel="PyLiveSessionSample")
                if issue.message:
                    omni.log.error(f"Issue: {issue.message}", channel="PyLiveSessionSample")
                if issue.at:
                    omni.log.error(f"At:    {issue.at.as_str()}", channel="PyLiveSessionSample")
                if issue.suggestion:
                    omni.log.error(f"Fix:   {issue.suggestion.message}", channel="PyLiveSessionSample")

        elif option == b"m":
            omni.log.info("Ending session and Merging live changes to root layer: ", channel="PyLiveSessionSample")
            end_and_merge_session(live_session)
            return

        elif option == b"q" or option == chr(27).encode():
            omni.log.info("Live edit complete", channel="PyLiveSessionSample")
            return

        else:
            omni.log.info(prompt_msg, channel="PyLiveSessionSample")


def channel_ticker(live_channel, quit_event):
    global g_stage_merged
    while not quit_event.is_set():
        messages = live_channel.processMessages()
        for message in messages:
            message_type_str = omni.connect.core.LiveSessionChannel.getMessageTypeName(message.type)
            omni.log.info(f"Channel Message: {message_type_str} {message.user.name} - {message.user.app}", channel="PyLiveSessionSample")

            if (
                message.type == omni.connect.core.LiveSessionChannel.MessageType.eMergeStarted
                or message.type == omni.connect.core.LiveSessionChannel.MessageType.eMergeFinished
            ):
                omni.log.warn("Exiting since a merge was initiated.", channel="PyLiveSessionSample")
                g_stage_merged = True

        time.sleep(0.0166)


def main():
    parser = argparse.ArgumentParser(description="Python Omniverse Client Sample", formatter_class=argparse.ArgumentDefaultsHelpFormatter)

    parser.add_argument("-v", "--verbose", action="store_true", default=False)
    parser.add_argument("-e", "--existing", action="store", required=True)
    parser.add_argument("-m", "--mesh", action="store", required=False, default="")

    args = parser.parse_args()

    stage_uri = args.existing
    search_mesh_str = args.mesh

    startOmniverse(args.verbose)

    if stage_uri and not omni.connect.core.isOmniUri(stage_uri):
        msg = (
            "This is not an Omniverse Nucleus URL: %s \n"
            "Correct Omniverse URL format is: omniverse://server_name/Path/To/Example/Folder/helloWorld_py.usd"
        )
        omni.log.error(msg % stage_uri, channel="PyLiveSessionSample")
        shutdownOmniverse()
        sys.exit(1)

    boxMesh = None
    stage = None

    omni.log.info(f"Stage URI: {stage_uri}", channel="PyLiveSessionSample")
    # Handle exception here because this will properly validate if we have a bad stage URL
    try:
        stage = Usd.Stage.Open(stage_uri)
    except Tf.ErrorException:
        # omni.log.error(traceback.format_exc(), channel="PyLiveSessionSample")
        omni.log.fatal(f"Failure to open stage in Omniverse: {stage_uri}, exiting.", channel="PyLiveSessionSample")
        sys.exit(1)

    boxMesh = findGeomMesh(stage, search_mesh_str)

    if not stage:
        shutdownOmniverse()
        omni.log.error("Unable to open stage", channel="PyLiveSessionSample")
        sys.exit(1)
    else:
        live_session = omni.connect.core.LiveSession.create(stage)
        if not live_session:
            omni.log.error(f"Failure to create a live session for stage: {stage_uri}", channel="PyLiveSessionSample")
            shutdownOmniverse()
            sys.exit(1)

        success = find_or_create_session(live_session)
        if not success:
            shutdownOmniverse()
            sys.exit(1)

    quit_event = threading.Event()
    quit_event.clear()
    tick_thread = threading.Thread(target=channel_ticker, args=(live_session.getChannel(), quit_event))
    tick_thread.start()

    if boxMesh is not None:
        run_live_edit(stage, boxMesh.GetPrim(), live_session)

    # tell the tick thread to stop processing channel messages and join
    quit_event.set()
    tick_thread.join()

    live_session = None
    shutdownOmniverse()


if __name__ == "__main__":
    main()
