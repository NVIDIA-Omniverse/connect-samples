-- Shared build scripts from repo_build package
repo_build = require("omni/repo/build")
repo_build.root = os.getcwd()

-- Shared build scripts for use in client Connectors
connect_build = require(path.replaceextension(os.matchfiles("_build/target-deps/omni_connect_sdk/*/dev/tools/premake/connect-sdk-public.lua")[1], ""))

workspace "Samples"
    connect_build.setup_workspace({
        msvc_version = "14.29.30133",
        winsdk_version = "10.0.18362.0",
    })

    -- override some connect_build settings
    -- install exeutables and libraries in the main target_build_dir
    target_bin_dir = target_build_dir
    target_lib_dir = target_build_dir


function sample(projectName, sourceFolder)
    project(projectName)

    includedirs { "source/common/include" }

    -- setup all paths, links, and carb dependencies to enable omni_connect_core
    connect_build.use_omni_client()
    connect_build.use_omni_resolver()
    connect_build.use_usd({
        "arch",
        "gf",
        "kind",
        "pcp",
        "plug",
        "sdf",
        "tf",
        "usd",
        "usdGeom",
        "usdLux",
        "usdPhysics",
        "usdShade",
        "usdSkel",
        "usdUtils",
        "vt",
    })
    connect_build.use_connect_core()

    filter { "system:windows" }
        -- This sets the working directory when debugging/running from Visual Studio
        debugdir "$(ProjectDir)..\\..\\.."
        filter { "configurations:debug" }
            debugenvs "CARB_APP_PATH=$(ProjectDir)..\\..\\..\\_build\\windows-x86_64\\debug"
        filter { "configurations:release" }
            debugenvs "CARB_APP_PATH=$(ProjectDir)..\\..\\..\\_build\\windows-x86_64\\release"
        filter {}
    filter { "system:linux" }
        links { "pthread", "stdc++fs" }
    filter {}

    connect_build.executable({
        name = projectName,
        sources = { "source/"..sourceFolder.."/**.*" },
    })
end

sample("HelloWorld", "helloWorld", true)
sample("LiveSession", "liveSession")
sample("omnicli", "omnicli")
sample("omniUsdaWatcher", "omniUsdaWatcher")
sample("omniSimpleSensor", "omniSimpleSensor")
sample("omniSensorThread", "omniSensorThread")
