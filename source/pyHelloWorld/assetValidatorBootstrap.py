# SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: MIT
#

# Python built-in
import os

# Python 3.8 - can't use PATH any longer
if hasattr(os, "add_dll_directory"):
    scriptdir = os.path.dirname(os.path.realpath(__file__))
    dlldir = os.path.abspath(os.path.join(scriptdir, "../../_build/windows-x86_64/release"))
    os.add_dll_directory(dlldir)

import omni.client
import omni.connect.core

# Omni imports
import omni_asset_validator


def main():
    # Add this material path so that the Asset Validator can resolve paths to the core MDL files like OmniPBR.mdl
    scriptdir = os.path.dirname(os.path.realpath(__file__))
    core_mdl_path = f'file:{os.path.abspath(os.path.join(scriptdir, "../../_build/target-deps/omni_core_materials/Base"))}'
    omni.client.add_default_search_path(core_mdl_path)

    # required in order to acquire carb plugins, which occurs during asset validator initialization
    omni.connect.core.startup()
    if not omni.connect.core.initialized():
        exit(1)

    omni_asset_validator.main()


if __name__ == "__main__":
    main()
