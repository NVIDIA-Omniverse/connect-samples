# SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: MIT
#

# Python built-in
import argparse
import math
import os
import sys
import traceback

# Python 3.8 - can't use PATH any longer
if hasattr(os, "add_dll_directory"):
    scriptdir = os.path.dirname(os.path.realpath(__file__))
    dlldir = os.path.abspath(os.path.join(scriptdir, "../../_build/windows-x86_64/release"))
    os.add_dll_directory(dlldir)

# Internal imports
import get_char_util
import material_util
import omni.client

# Omni imports
import omni.connect.core
import omni.log
import omni.usd_resolver

# USD imports
from pxr import Gf, Kind, Sdf, Tf, Usd, UsdGeom, UsdLux, UsdPhysics, UsdShade, UsdUtils

# Globals
g_connection_status_subscription = None
g_stage = None
default_path = "omniverse://localhost/Users/{user_name}/"
h = 50.0
boxVertexIndices = [0, 1, 2, 1, 3, 2, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11, 12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23]
boxVertexCounts = [3] * 12
boxNormals = [
    (0, 0, -1),
    (0, 0, -1),
    (0, 0, -1),
    (0, 0, -1),
    (0, 0, 1),
    (0, 0, 1),
    (0, 0, 1),
    (0, 0, 1),
    (0, -1, 0),
    (0, -1, 0),
    (0, -1, 0),
    (0, -1, 0),
    (1, 0, 0),
    (1, 0, 0),
    (1, 0, 0),
    (1, 0, 0),
    (0, 1, 0),
    (0, 1, 0),
    (0, 1, 0),
    (0, 1, 0),
    (-1, 0, 0),
    (-1, 0, 0),
    (-1, 0, 0),
    (-1, 0, 0),
]
boxPoints = [
    (h, -h, -h),
    (-h, -h, -h),
    (h, h, -h),
    (-h, h, -h),
    (h, h, h),
    (-h, h, h),
    (-h, -h, h),
    (h, -h, h),
    (h, -h, h),
    (-h, -h, h),
    (-h, -h, -h),
    (h, -h, -h),
    (h, h, h),
    (h, -h, h),
    (h, -h, -h),
    (h, h, -h),
    (-h, h, h),
    (h, h, h),
    (h, h, -h),
    (-h, h, -h),
    (-h, -h, h),
    (-h, h, h),
    (-h, h, -h),
    (-h, -h, -h),
]
boxUVs = [
    (0, 0),
    (0, 1),
    (1, 1),
    (1, 0),
    (0, 0),
    (0, 1),
    (1, 1),
    (1, 0),
    (0, 0),
    (0, 1),
    (1, 1),
    (1, 0),
    (0, 0),
    (0, 1),
    (1, 1),
    (1, 0),
    (0, 0),
    (0, 1),
    (1, 1),
    (1, 0),
    (0, 0),
    (0, 1),
    (1, 1),
    (1, 0),
]


def connectionStatusCallback(url, connectionStatus):
    if connectionStatus not in (
        omni.client.ConnectionStatus.CONNECTING,
        omni.client.ConnectionStatus.CONNECTED,
    ):
        omni.log.fatal(f"Error connecting to Nucleus URL: <{url}> (OmniClientConnectionStatus: {connectionStatus}).", channel="PyHelloWorld")


def startOmniverse(logging_enabled=False):
    if not omni.connect.core.startup():
        omni.log.fatal("Unable to initialize Omniverse client, exiting.", channel="PyHelloWorld")
        sys.exit(1)

    # Set the retry behavior to limit retries so that invalid server addresses fail quickly
    omni.client.set_retries(0, 0, 0)

    omni.log.get_log().level = omni.log.Level.VERBOSE if logging_enabled else omni.log.Level.INFO

    global g_connection_status_subscription
    g_connection_status_subscription = omni.client.register_connection_status_callback(connectionStatusCallback)


def shutdownOmniverse():
    omni.client.live_wait_for_pending_updates()


def createOmniverseModel(path, live_edit, usda_output):
    omni.log.info("Creating Omniverse stage", channel="PyHelloWorld")
    global g_stage

    # Use a "".live" extension for live updating, otherwise use a ".usd" extension
    ext = ".usda" if usda_output else ".usd"
    stageUrl = path + "/helloworld_py" + (".live" if live_edit else ext)

    if omni.connect.core.isOmniUri(path):
        # We test and create the directory first - making sure we have access to the directory
        if not omni.connect.core.doesUriExist(path):
            result = omni.client.create_folder(path)
            # This may be the first client call that accesses a server, let the user know if there's a connection error here
            if result == omni.client.Result.ERROR_CONNECTION:
                omni.log.fatal(f"<{result}>: Error connecting to Nucleus: {stageUrl}, exiting.", channel="PyHelloWorld")
                sys.exit(1)
            elif result != omni.client.Result.OK:
                omni.log.fatal(f"<{result}>: Error creating the directory: {path}, exiting.", channel="PyHelloWorld")
                sys.exit(1)

        # If the directory exists, we need to test if we have access to it
        elif not omni.connect.core.isUriWritable(path):
            omni.log.fatal(f"No write access to {path}, exiting.", channel="PyHelloWorld")
            sys.exit(1)

        # Delete the existing file
        elif omni.connect.core.doesUriExist(stageUrl):
            result = omni.client.delete(stageUrl)
            if result != omni.client.Result.OK:
                omni.log.fatal(f"<{result}>: Error deleting the stage: {stageUrl}, exiting.", channel="PyHelloWorld")
                sys.exit(1)

    omni.log.info(f"Creating stage: {stageUrl}", channel="PyHelloWorld")

    # Handle exception here because this will properly validate if we have a bad stage URL
    try:
        g_stage = omni.connect.core.createStage(
            identifier=stageUrl,
            defaultPrimName="World",
            upAxis=UsdGeom.Tokens.y,
            linearUnits=UsdGeom.LinearUnits.centimeters,
        )
        if not g_stage:
            omni.log.fatal(f"Error creating stage {stageUrl}, exiting.", channel="PyHelloWorld")
            sys.exit(1)

    except Tf.ErrorException:
        omni.log.error(traceback.format_exc(), channel="PyHelloWorld")
        omni.log.fatal(f"Error creating stage: {stageUrl}, exiting.", channel="PyHelloWorld")
        sys.exit(1)

    omni.log.info(f"Created stage: {stageUrl}", channel="PyHelloWorld")

    # Redefine the defaultPrim as an Xform, as `createStage` authored a Scope
    xform = omni.connect.core.defineXform(g_stage, g_stage.GetDefaultPrim().GetPath())

    if not xform:
        omni.log.fatal(f"Failure to redefine default prim - {g_stage.GetDefaultPrim().GetPath()} as Xform", channel="PyHelloWorld")
        sys.exit(1)

    # Set the default prim as an assembly to support using component references
    Usd.ModelAPI(xform).SetKind(Kind.Tokens.assembly)

    return stageUrl


def createPhysicsScene():
    global g_stage
    default_prim_path = g_stage.GetDefaultPrim().GetPath().pathString

    sceneName = "/physicsScene"
    scenePrimPath = default_prim_path + sceneName

    # Create physics scene, note that we dont have to specify gravity
    # the default value is derived based on the scene up Axis and meters per unit.
    # Hence in this case the gravity would be (0.0, -981.0, 0.0) since we have
    # defined the Y up-axis and we are having a scene in centimeters.
    UsdPhysics.Scene.Define(g_stage, scenePrimPath)


def enablePhysics(prim, dynamic):
    if dynamic:
        # Make the cube a physics rigid body dynamic
        UsdPhysics.RigidBodyAPI.Apply(prim)

    # Add collision
    collision_api = UsdPhysics.CollisionAPI.Apply(prim)
    if not collision_api:
        omni.log.fatal(
            "Failed to apply UsdPhysics.CollisionAPI, " "check that the UsdPhysics plugin is located in the USD plugins folders",
            channel="PyHelloWorld",
        )
        sys.exit(1)

    if prim.IsA(UsdGeom.Mesh):
        meshCollisionAPI = UsdPhysics.MeshCollisionAPI.Apply(prim)
        if dynamic:
            # set mesh approximation to convexHull for dynamic meshes
            meshCollisionAPI.CreateApproximationAttr().Set(UsdPhysics.Tokens.convexHull)
        else:
            # set mesh approximation to none - triangle mesh as is will be used
            meshCollisionAPI.CreateApproximationAttr().Set(UsdPhysics.Tokens.none)


# create dynamic cube
def createDynamicCube(size):
    global g_stage
    # Create the geometry under the default prim
    defaultPrim = g_stage.GetDefaultPrim()

    cubeName = "cube"
    # Note we use omni.connect.core.getValidPrimName to sanitize the name. Make sure it's a valid prim name.
    cubePrimPath = defaultPrim.GetPath().AppendChild(omni.connect.core.getValidPrimName(cubeName))

    cube = UsdGeom.Cube.Define(g_stage, cubePrimPath)

    if not cube:
        omni.log.fatal("Failure to create cube", channel="PyHelloWorld")
        sys.exit(1)

    # Set the display name as the preferred name
    omni.connect.core.setDisplayName(cube.GetPrim(), cubeName)

    # Move it up
    cube.AddTranslateOp().Set(Gf.Vec3f(65.0, 300.0, 65.0))

    cube.GetSizeAttr().Set(size)
    cube.CreateExtentAttr(size * 0.5 * cube.GetExtentAttr().Get())

    enablePhysics(cube.GetPrim(), True)

    # Make the kind a component to support the assembly/component selection hierarchy
    Usd.ModelAPI(cube.GetPrim()).SetKind(Kind.Tokens.component)

    # Commit the changes to the USD
    save_stage(comment="Created a dynamic cube.")


# create cylinders. This is for demonstrating omni.connect.core.getValidChildNames and omni.connect.core.getValidPrimName
def createCylinders(size):
    global g_stage
    # Create the geometry under the default prim
    defaultPrim = g_stage.GetDefaultPrim()

    xformName = "1Cylinders"

    # Note we use omni.connect.core.getValidPrimName to get a valid prim name - This will prepend a underscore in front of 1 - _1Cylinder
    xform = omni.connect.core.defineXform(defaultPrim, omni.connect.core.getValidPrimName(xformName))

    if not xform:
        omni.log.fatal("Failure to create xform", channel="PyHelloWorld")
        sys.exit(1)

    cylinderNames = ["1cylinder", "1cylinder", "1cylinder"]

    # omni.connect.core.getValidChildNames will return a list of valid name (no leading digits, no naming conflicts)
    for idx, name in enumerate(omni.connect.core.getValidChildNames(xform.GetPrim(), cylinderNames)):
        cylinderPath = xform.GetPrim().GetPath().AppendChild(name)
        cylinder = UsdGeom.Cylinder.Define(g_stage, cylinderPath)

        if not cylinder:
            omni.log.fatal("Failure to create cylinder", channel="PyHelloWorld")
            sys.exit(1)

        # Set the display name as the preferred name
        omni.connect.core.setDisplayName(cylinder.GetPrim(), cylinderNames[idx])

        cylinderXform = Gf.Transform()
        # Scale it
        cylinderXform.SetScale(Gf.Vec3d(size * (idx + 1)))
        # Move it up
        cylinderXform.SetTranslation(Gf.Vec3d(350, 100.0 * idx, 350))
        omni.connect.core.setLocalTransform(cylinder.GetPrim(), cylinderXform)

    # Commit the changes to the USD
    save_stage(comment="Add cylinders")


# Create a simple quad in USD with normals and add a collider
def createQuad(size):
    global g_stage

    # Create the geometry under the default prim
    defaultPrim = g_stage.GetDefaultPrim()
    quadName = "quad"
    # Note we use omni.connect.core.getValidPrimName to sanitize the name. Make sure it's a valid prim name.
    quadPrimPath = defaultPrim.GetPath().AppendChild(omni.connect.core.getValidPrimName(quadName))

    # Add all of the vertices
    points = [Gf.Vec3f(-size, 0.0, -size), Gf.Vec3f(-size, 0.0, size), Gf.Vec3f(size, 0.0, size), Gf.Vec3f(size, 0.0, -size)]

    # Calculate indices for each triangle
    vecIndices = [0, 1, 2, 3]

    # Add vertex normals
    normalsPrimvarData = omni.connect.core.Vec3fPrimvarData(UsdGeom.Tokens.vertex, [Gf.Vec3f(0.0, 1.0, 0.0)] * 4)
    normalsPrimvarData.index()

    # Add face vertex count
    faceVertexCounts = [4]

    mesh = omni.connect.core.definePolyMesh(g_stage, quadPrimPath, faceVertexCounts, vecIndices, points, normals=normalsPrimvarData)

    if not mesh:
        omni.log.fatal("Failure to create quad", channel="PyHelloWorld")
        sys.exit(1)

    # Set the display name as the preferred name
    omni.connect.core.setDisplayName(mesh.GetPrim(), quadName)

    # set is as a static triangle mesh
    enablePhysics(mesh.GetPrim(), False)

    # Make the kind a component to support the assembly/component selection hierarchy
    Usd.ModelAPI(mesh.GetPrim()).SetKind(Kind.Tokens.component)

    material_util.createAndBindMaterial(mesh.GetPrim(), "QuadMat", color=Gf.Vec3f(0.025, 0.025, 0.025), roughness=0.04)

    # Commit the changes to the USD
    save_stage(comment="Created a Quad.")


def save_stage(comment=""):
    global g_stage

    omni.connect.core.saveStage(g_stage, comment)
    omni.client.live_process()


def createBox(boxNumber=0):
    global g_stage
    defaultPrim = g_stage.GetDefaultPrim()

    name = f"box-{boxNumber}"
    # Note that omni.connect.core.getValidPrimName will change the hyphen to an underscore.
    boxPath = defaultPrim.GetPath().AppendChild(omni.connect.core.getValidPrimName(name))

    normalsPrimvarData = omni.connect.core.Vec3fPrimvarData(UsdGeom.Tokens.vertex, boxNormals)
    normalsPrimvarData.index()
    uvsPrimvarData = omni.connect.core.Vec2fPrimvarData(UsdGeom.Tokens.vertex, boxUVs)
    uvsPrimvarData.index()
    box = omni.connect.core.definePolyMesh(
        g_stage,
        boxPath,
        boxVertexCounts,
        boxVertexIndices,
        boxPoints,
        normals=normalsPrimvarData,
        uvs=uvsPrimvarData,
        displayColor=omni.connect.core.Vec3fPrimvarData(UsdGeom.Tokens.constant, [Gf.Vec3f(0.463, 0.725, 0.0)]),
    )
    if not box:
        omni.log.fatal("Failure to create box", channel="PyHelloWorld")
        sys.exit(1)

    # Set the display name as the preferred name: {box-boxNumber}
    omni.connect.core.setDisplayName(box.GetPrim(), name)

    # Set init transformation
    omni.connect.core.setLocalTransform(
        box.GetPrim(),
        translation=Gf.Vec3d(0.0, 100.0, 0.0),
        pivot=Gf.Vec3d(0.0),
        rotation=Gf.Vec3f(20.0, 0.0, 20.0),
        rotationOrder=omni.connect.core.RotationOrder.eXyz,
        scale=Gf.Vec3f(1),
        time=Usd.TimeCode.Default(),
    )

    enablePhysics(box.GetPrim(), True)

    # Make the kind a component to support the assembly/component selection hierarchy
    Usd.ModelAPI(box.GetPrim()).SetKind(Kind.Tokens.component)

    save_stage(comment="Created a box.")

    return box


def findGeomMesh(existing_stage, boxNumber=0):
    global g_stage
    omni.log.verbose(existing_stage, channel="PyHelloWorld")

    g_stage = Usd.Stage.Open(existing_stage)

    if not g_stage:
        omni.log.fatal(f"Unable to open stage {existing_stage}", channel="PyHelloWorld")
        sys.exit(1)

    # meshPrim = stage.GetPrimAtPath('/World/box_%d' % boxNumber)
    for node in g_stage.Traverse():
        if node.IsA(UsdGeom.Mesh):
            return UsdGeom.Mesh(node)

    omni.log.fatal(f"No UsdGeomMesh found in stage {existing_stage}", channel="PyHelloWorld")
    sys.exit(1)


def uploadReferences(destination_path):
    # Materials
    uriPath = destination_path + "/Materials"
    omni.client.delete(uriPath)
    omni.client.copy("resources/Materials", uriPath)

    # Referenced Props
    uriPath = destination_path + "/Props"
    omni.client.delete(uriPath)
    omni.client.copy("resources/Props", uriPath)


# Create references and modify an OmniPBR material
def createReferenceAndPayload(stageUrl):
    # the referenced prop is in /Props/Box/boxu.usda
    global g_stage
    default_prim = g_stage.GetDefaultPrim()

    xform_name = omni.connect.core.getValidPrimName("Box_Reference")
    box_xform = omni.connect.core.defineXform(default_prim, xform_name)
    if not box_xform:
        omni.log.fatal(f"Failure to create xform - {xform_name}", channel="PyHelloWorld")
        sys.exit(1)

    # create a reference
    box_xform_prim = box_xform.GetPrim()
    box_xform_prim.GetReferences().AddReference("./Props/Box/box.usda")
    enablePhysics(box_xform_prim, True)
    UsdPhysics.RigidBodyAPI.Apply(box_xform_prim).CreateAngularVelocityAttr(Gf.Vec3f(0, 1000, 0))

    # Set srt transform
    omni.connect.core.setLocalTransform(
        box_xform_prim,
        translation=Gf.Vec3d(200, 100, -200),
        pivot=Gf.Vec3d(0.0),
        rotation=Gf.Vec3f(3, 0, 8),
        rotationOrder=omni.connect.core.RotationOrder.eXyz,
        scale=Gf.Vec3f(0.5),
        time=Usd.TimeCode.Default(),
    )

    # create a payload payload
    xform_name = omni.connect.core.getValidPrimName("Box_Payload")
    box_xform = omni.connect.core.defineXform(default_prim, xform_name)
    if not box_xform:
        omni.log.fatal(f"Failure to create xform - {xform_name}", channel="PyHelloWorld")
        sys.exit(1)

    box_xform_prim = box_xform.GetPrim()
    box_xform_prim.GetPayloads().AddPayload("./Props/Box/box.usda")
    enablePhysics(box_xform_prim, True)
    UsdPhysics.RigidBodyAPI.Apply(box_xform_prim).CreateAngularVelocityAttr(Gf.Vec3f(-1000, 0, 0))
    # Set srt transform
    omni.connect.core.setLocalTransform(
        box_xform_prim,
        translation=Gf.Vec3d(-200, 180, 200),
        pivot=Gf.Vec3d(0.0),
        rotation=Gf.Vec3f(-4, 90, 8),
        rotationOrder=omni.connect.core.RotationOrder.eXyz,
        scale=Gf.Vec3f(0.5),
        time=Usd.TimeCode.Default(),
    )

    # Modify the payload's material in Box_Payload/Looks/Fieldstone
    material_prim_path = default_prim.GetPath().AppendPath(Sdf.Path("Box_Payload/Looks/Fieldstone"))
    material_prim = UsdShade.Material.Get(g_stage, material_prim_path)
    omni.connect.core.createMdlShaderInput(material_prim, "diffuse_tint", Gf.Vec3f(1, 0.1, 0), Sdf.ValueTypeNames.Color3f)

    # We could just save the stage here, but we'll learn about CoalescingDiagnosticDelegate first...
    #  We collect all of the warnings/errors from the USD warnings stream and only print if
    #  there's a larger issue than the "crate file upgrade" WARNING that is emitted
    delegate = UsdUtils.CoalescingDiagnosticDelegate()
    save_stage(comment="Added Reference, Payload, and modified OmniPBR")
    stageSaveDiagnostics = delegate.TakeUncoalescedDiagnostics()
    if len(stageSaveDiagnostics) > 1:
        for diag in stageSaveDiagnostics:
            msg = f"In {diag.sourceFunction} at line {diag.sourceLineNumber} of " f"{diag.sourceFileName} -- {diag.commentary}"
            if "ERROR" in diag.diagnosticCodeString:
                omni.log.error(msg)
            else:
                omni.log.warning(msg)


# Create a distant light in the scene.
def createDistantLight():
    global g_stage
    default_prim_path = g_stage.GetDefaultPrim().GetPath().pathString
    newLight = UsdLux.DistantLight.Define(g_stage, default_prim_path + "/DistantLight")
    angleValue = 0.53
    colorValue = Gf.Vec3f(1.0, 1.0, 0.745)
    intensityValue = 500.0

    newLight.CreateIntensityAttr(intensityValue)
    newLight.CreateAngleAttr(angleValue)
    newLight.CreateColorAttr(colorValue)

    # Also write the new UsdLux Schema attributes if using an old USD lib (pre 21.02)
    if newLight.GetPrim().HasAttribute("intensity"):
        newLight.GetPrim().CreateAttribute("inputs:intensity", Sdf.ValueTypeNames.Float, custom=False).Set(intensityValue)
        newLight.GetPrim().CreateAttribute("inputs:angle", Sdf.ValueTypeNames.Float, custom=False).Set(angleValue)
        newLight.GetPrim().CreateAttribute("inputs:color", Sdf.ValueTypeNames.Color3f, custom=False).Set(colorValue)
    else:  # or write the old UsdLux Schema attributes if using a new USD lib (post 21.02)
        newLight.GetPrim().CreateAttribute("intensity", Sdf.ValueTypeNames.Float, custom=False).Set(intensityValue)
        newLight.GetPrim().CreateAttribute("angle", Sdf.ValueTypeNames.Float, custom=False).Set(angleValue)
        newLight.GetPrim().CreateAttribute("color", Sdf.ValueTypeNames.Color3f, custom=False).Set(colorValue)

    # Rotate the distant light
    # There are multiple approaches to setting transforms - we'll use the GfRotation API in this case,
    # but you could also use the TRS method seen in other functions here, or by constructing a 4x4 matrix.
    # Note that these methods work across all prims, not just lights.
    lightXform = omni.connect.core.getLocalTransform(newLight.GetPrim())

    # Create a rotation
    rotation = Gf.Rotation(Gf.Vec3d.XAxis(), 139) * Gf.Rotation(Gf.Vec3d.YAxis(), 44) * Gf.Rotation(Gf.Vec3d.ZAxis(), 190)

    # This quaternion is equivalent to the above
    # rotation = Gf.Rotation(Gf.Quatd(0.32124, -0.20638, 0.85372, 0.35405))

    # Apply the rotation to the xform
    lightXform.SetRotation(rotation)

    # Last, set the xform to the prim
    omni.connect.core.setLocalTransform(newLight.GetPrim(), lightXform)

    # Alternatively you could use this Connect SDK method to do all of the above in a single call:
    # omni.connect.core.setLocalTransform(
    #     newLight.GetPrim(),
    #     translation=Gf.Vec3d(0.0),
    #     pivot=Gf.Vec3d(0.0),
    #     rotation=Gf.Vec3f(139, 44, 190),
    #     rotationOrder=omni.connect.core.RotationOrder.eXyz,
    #     scale=Gf.Vec3f(1),
    #     time=Usd.TimeCode.Default(),
    # )

    # Or create a 4x4 matrix, which provides a few other tools
    # matrix = Gf.Matrix4d()
    # matrix.SetRotate(rotation)
    # omni.connect.core.setLocalTransform(newLight.GetPrim(), matrix)

    # Make the kind a component to support the assembly/component selection hierarchy
    Usd.ModelAPI(newLight.GetPrim()).SetKind(Kind.Tokens.component)

    save_stage(comment="Created a DistantLight.")


# Create a dome light in the scene.
def createDomeLight(texturePath):
    global g_stage
    default_prim_path = g_stage.GetDefaultPrim().GetPath().pathString
    newLight = UsdLux.DomeLight.Define(g_stage, default_prim_path + "/DomeLight")
    intensityValue = 900.0
    newLight.CreateIntensityAttr(intensityValue)
    newLight.CreateTextureFileAttr(texturePath)
    newLight.CreateTextureFormatAttr(UsdLux.Tokens.latlong)

    # Also write the new UsdLux Schema attributes if using an old USD lib (pre 21.02)
    if newLight.GetPrim().HasAttribute("intensity"):
        newLight.GetPrim().CreateAttribute("inputs:intensity", Sdf.ValueTypeNames.Float, custom=False).Set(intensityValue)
        newLight.GetPrim().CreateAttribute("inputs:texture:file", Sdf.ValueTypeNames.Asset, custom=False).Set(texturePath)
        newLight.GetPrim().CreateAttribute("inputs:texture:format", Sdf.ValueTypeNames.Token, custom=False).Set(UsdLux.Tokens.latlong)
    else:
        newLight.GetPrim().CreateAttribute("intensity", Sdf.ValueTypeNames.Float, custom=False).Set(intensityValue)
        newLight.GetPrim().CreateAttribute("texture:file", Sdf.ValueTypeNames.Asset, custom=False).Set(texturePath)
        newLight.GetPrim().CreateAttribute("texture:format", Sdf.ValueTypeNames.Token, custom=False).Set(UsdLux.Tokens.latlong)

    # Set rotation on domelight
    xForm = newLight
    rotateOp = xForm.AddXformOp(UsdGeom.XformOp.TypeRotateXYZ, UsdGeom.XformOp.PrecisionDouble)
    rotateOp.Set(Gf.Vec3d(270, 270, 0))

    # Make the kind a component to support the assembly/component selection hierarchy
    Usd.ModelAPI(newLight.GetPrim()).SetKind(Kind.Tokens.component)

    save_stage(comment="Created a DomeLight.")


def createCamera():
    # Construct path for the camera under the default prim
    defaultPrimPath = g_stage.GetDefaultPrim().GetPath()
    cameraPath = defaultPrimPath.AppendChild("Camera")

    # GfCamera is a container for camera attributes.
    # The connect sdk defineCamera function takes it as an argument.
    gfCam = Gf.Camera()

    # Put the camera about a 1000 units from the origin, and focus on
    # the textured cube we created in a previous function
    gfCam.focusDistance = 1000.0
    gfCam.focalLength = 35.0
    gfCam.fStop = 0.5
    gfCam.horizontalAperture = 15.0

    # Define the camera
    newCamera = omni.connect.core.defineCamera(g_stage, cameraPath, gfCam)

    # We could configure the xform in the GfCamera, but we can also do so with:
    omni.connect.core.setLocalTransform(
        newCamera.GetPrim(),
        translation=Gf.Vec3d(932.84, 96.0, -453.57),
        pivot=Gf.Vec3d(0.0),
        rotation=Gf.Vec3f(-178.52, 66.03, -180.0),
        rotationOrder=omni.connect.core.RotationOrder.eXyz,
        scale=Gf.Vec3f(1),
        time=Usd.TimeCode.Default(),
    )


def createNoBoundsCube(size):
    global g_stage
    defaultPrim = g_stage.GetDefaultPrim()

    name = "no_bounds_quad"
    # Note we use omni.connect.core.getValidPrimName to sanitize the name. Make sure it's a valid prim name.
    meshPrimPath = defaultPrim.GetPath().AppendChild(omni.connect.core.getValidPrimName(name))

    omni.log.info(f"Adding a quad with no extents to generate a validation failure: {meshPrimPath}", channel="PyHelloWorld")

    mesh = UsdGeom.Mesh.Define(g_stage, meshPrimPath)
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.catmullClark)

    if not mesh:
        omni.log.fatal("Failure to create No Bounds Cube", channel="PyHelloWorld")
        sys.exit(1)

    # Set the display name as the preferred name
    omni.connect.core.setDisplayName(mesh.GetPrim(), name)

    # Add all of the vertices
    points = [Gf.Vec3f(-size, 0.0, -size), Gf.Vec3f(-size, 0.0, size), Gf.Vec3f(size, 0.0, size), Gf.Vec3f(size, 0.0, -size)]
    mesh.CreatePointsAttr(points)

    # Not doing this intentionally :-)
    # mesh.CreateExtentAttr(mesh.ComputeExtent(points))

    # Calculate indices for each triangle
    vecIndices = [0, 1, 2, 3]
    mesh.CreateFaceVertexIndicesAttr(vecIndices)

    # Add face vertex count
    faceVertexCounts = [4]
    mesh.CreateFaceVertexCountsAttr(faceVertexCounts)

    # Commit the changes to the USD
    save_stage(comment="Created a cube with no extents to fail asset.")


def createEmptyFolder(emptyFolderPath):
    omni.log.info(f"Creating new folder: {emptyFolderPath}")
    result = omni.client.create_folder(emptyFolderPath)

    omni.log.info(f"Finished (this may be an error if the folder already exists) [ {result.name} ]", channel="PyHelloWorld")


def run_live_edit(prim, stageUrl):
    global g_stage
    angle = 0
    omni.client.live_process()
    prim_path = prim.GetPath()
    main_option_msg = "Enter 't' to transform,\n" "'m' to send a channel message,\n" "'l' to leave the channel,\n" "'q' to quit.\n"
    omni.log.info(f"Begin Live Edit on {prim_path} - \n{main_option_msg}", channel="PyHelloWorld")

    # Message channel callback responsds to channel events
    def message_channel_callback(result: omni.client.Result, channel_event: omni.client.ChannelEvent, user_id: str, content: omni.client.Content):
        omni.log.info(f"Channel event: {channel_event}", channel="PyHelloWorld")
        if channel_event == omni.client.ChannelEvent.MESSAGE:
            # Assume that this is an ASCII message from another client
            text_message = memoryview(content).tobytes().decode("ascii")
            omni.log.info(f"Channel message received: {text_message}", channel="PyHelloWorld")

    # We aren't doing anything in particular when the channel messages are finished sending
    def on_send_message_cb(result):
        pass

    # Join a message channel to communicate text messages between clients
    join_request = omni.client.join_channel_with_callback(stageUrl + ".__omni_channel__", message_channel_callback)

    while True:
        option = get_char_util.getChar()

        omni.client.live_process()
        if option == b"t":
            angle = (angle + 15) % 360
            radians = angle * 3.1415926 / 180.0
            x = math.sin(radians) * 100.0
            y = math.cos(radians) * 100.0

            # Get srt transform from prim
            translate, pivot, rot_xyz, rotationOrder, scale = omni.connect.core.getLocalTransformComponents(prim, time=Usd.TimeCode.Default())

            # Translate and rotate
            translate += Gf.Vec3d(x, 0.0, y)
            rot_xyz = Gf.Vec3f(rot_xyz[0], angle, rot_xyz[2])

            omni.log.info(
                f"Setting pos [{translate[0]:.2f}, {translate[1]:.2f}, {translate[2]:.2f}] and rot [{rot_xyz[0]:.2f}, "
                f"{rot_xyz[1]:.2f}, {rot_xyz[2]:.2f}]",
                channel="PyHelloWorld",
            )

            # Set srt transform
            omni.connect.core.setLocalTransform(
                prim,
                translation=translate,
                pivot=pivot,
                rotation=rot_xyz,
                rotationOrder=omni.connect.core.RotationOrder.eXyz,
                scale=scale,
                time=Usd.TimeCode.Default(),
            )

            save_stage()

        elif option == b"m":
            if join_request:
                omni.log.info("Enter a channel message: ", channel="PyHelloWorld")
                message = input()
                omni.client.send_message_with_callback(join_request.id, message.encode("ascii"), on_send_message_cb)
            else:
                omni.log.info("The message channel is disconnected.", channel="PyHelloWorld")

        elif option == b"l":
            omni.log.info("Leaving message channel", channel="PyHelloWorld")
            if join_request:
                join_request.stop()
                join_request = None

        elif option == b"q" or option == chr(27).encode():
            omni.log.info("Live edit complete", channel="PyHelloWorld")
            break

        else:
            omni.log.info(main_option_msg, channel="PyHelloWorld")


def main(args):
    existing_stage = args.existing
    live_edit = args.live or bool(existing_stage)
    destination_path = args.path
    logging_enabled = args.verbose
    insert_validation_failure = args.fail
    final_checkpoint_comment = ""
    usda_output = args.usda

    startOmniverse(logging_enabled=logging_enabled)

    # Handle default destination_path
    if not existing_stage and destination_path == default_path:
        omni.log.info("Default output path specified. Checking the localhost Users folder for a valid username", channel="PyHelloWorld")
        user_folder = os.path.dirname(os.path.dirname(default_path))
        user_name = omni.connect.core.getUser(user_folder)
        if not user_name:
            omni.log.error(f"Cannot access directory: {user_folder}", channel="PyHelloWorld")
            exit(1)
        destination_path = destination_path.format(user_name=user_name)
    elif existing_stage:
        destination_path = existing_stage

    # Make sure the given path is a Omniverse Nucleus path for live editing.
    if live_edit:
        if (destination_path and not omni.connect.core.isOmniUri(destination_path)) or (
            existing_stage and not omni.connect.core.isOmniUri(existing_stage)
        ):
            msg = (
                "This is not an Omniverse Nucleus URL: %s\n"
                "Live Edit (--live/-l) is only supported in Omniverse Nucleus.\n"
                "Correct Omniverse URL format is: omniverse://server_name/Path/To/Example/Folder\n"
            )
            omni.log.error(msg % destination_path, channel="PyHelloWorld")
            exit(1)

    boxMesh = None

    # Create new
    if not existing_stage:
        # Create the USD model in Omniverse
        stageUrl = createOmniverseModel(destination_path, live_edit, usda_output)

        # Log the username for the server
        omni.log.info(f"Connected username: {omni.connect.core.getUser(stageUrl)}", channel="PyHelloWorld")

        # Create physics scene
        createPhysicsScene()

        # Create box geometry in the model
        boxMesh = createBox()

        # Create dynamic cube
        createDynamicCube(100.0)

        # If requested, create a cube with no bounds, creating a validation error
        if insert_validation_failure:
            createNoBoundsCube(50.0)

        # Create quad - static tri mesh collision so that the box collides with it
        createQuad(500.0)

        # Create cylinders - This demonstrate how the display name and valid name work.
        createCylinders(20)

        # Create a distance and dome light in the scene
        createDistantLight()
        createDomeLight("./Materials/kloofendal_48d_partly_cloudy.hdr")

        # Create a camera
        createCamera()

        # Upload a material, textures, and props to the Omniverse server
        uploadReferences(destination_path)

        # Add a material to the box
        material_util.createAndBindMaterial(
            boxMesh.GetPrim(),
            "Fieldstone",
            color=Gf.Vec3f(0.4, 0.3, 0.2),
            diffuseTexturePath="./Materials/Fieldstone/Fieldstone_BaseColor.png",
            normalTexturePath="./Materials/Fieldstone/Fieldstone_N.png",
            ormTexturePath="./Materials/Fieldstone/Fieldstone_ORM.png",
        )
        save_stage(comment="Add material to the box")

        # Add a reference, payload and modify OmniPBR
        createReferenceAndPayload(stageUrl)

        # Create an empty folder, just as an example
        createEmptyFolder(destination_path + "/EmptyFolder")

        # Add checkpoint comment
        omni.connect.core.createUriCheckpoint(stageUrl, final_checkpoint_comment)
    else:
        if omni.connect.core.isOmniUri(destination_path):
            if not omni.connect.core.doesUriExist(destination_path):
                omni.log.fatal(f"{destination_path} does not exist.")
                sys.exit(1)
            elif not omni.connect.core.isUriWritable(destination_path):
                omni.log.warn(f"{destination_path} is not writable. Allowing program to continue, all changes will be discarded when exiting.")

        stageUrl = existing_stage
        omni.log.info(f"Stage url: {stageUrl}", channel="PyHelloWorld")
        boxMesh = findGeomMesh(existing_stage)

        # If requested, create a cube with no bounds, creating a validation error, then exit
        if insert_validation_failure:
            createNoBoundsCube(50.0)
            shutdownOmniverse()
            sys.exit()

    if not boxMesh:
        omni.log.fatal("Unable to create or find mesh", channel="PyHelloWorld")
        sys.exit(1)
    else:
        omni.log.info("Mesh created/found successfully", channel="PyHelloWorld")

    if live_edit and boxMesh is not None:
        run_live_edit(boxMesh.GetPrim(), stageUrl)

    shutdownOmniverse()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Python Omniverse Client Sample", formatter_class=argparse.ArgumentDefaultsHelpFormatter)

    parser.add_argument("-a", "--usda", action="store_true", default=False, help="Save the output stage in ASCII format (.usda)")
    parser.add_argument(
        "-l",
        "--live",
        action="store_true",
        default=False,
        help="Allow the user to continue modifying the stage live after creating (with the 't' key)",
    )
    parser.add_argument("-p", "--path", action="store", default=default_path, help="Alternate destination stage path folder")
    parser.add_argument("-v", "--verbose", action="store_true", default=False, help="Show the verbose Omniverse logging")
    parser.add_argument("-e", "--existing", action="store", help="Open an existing stage and perform live transform edits (full omniverse URL)")
    parser.add_argument("-f", "--fail", action="store_true", default=False, help="Create a cube with no bounds, creating a validation error")

    args = parser.parse_args()
    main(args)
