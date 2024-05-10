# SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: MIT

# This makefile is a simple example for a Connector, application, or converter for including, linking, and executing with
# OpenUSD and the Connect SDK
#
# By default it will build against the release version of OpenUSD, to build against the debug version run `make CONFIG=debug`.

# The expectation is that USD, Connect SDK, and other dependencies are present in the `./connect-sdk/target-deps` directory
BOOSTVER = boost-1_78
DEPSDIR = $(CURDIR)/connect-sdk/target-deps
PYTHONVER = python3.10
PROGRAMNAME = UsdTraverse

ifndef CONFIG
	CONFIG=release
endif

ifndef TARGETDIR
	TARGETDIR = $(CURDIR)/$(CONFIG)
endif

# Debug vs. Release differences
ifeq ($(CONFIG),debug)
	CONFIG_DEFINES += -g -DDEBUG -O0 -DTBB_USE_DEBUG=1
else ifeq ($(CONFIG),release)
	CONFIG_DEFINES += -DNDEBUG -O2
endif

# ABI Settings
ifndef ABI_DEFINES
	ABI_DEFINES = -D_GLIBCXX_USE_CXX11_ABI=0 -std=c++17
endif

# Ignored Warnings
ifndef IGNORED_WARNINGS
	IGNORED_WARNINGS = -Wno-deprecated
endif

# Include search directories
 OMNI_USD_INCLUDE_DIRS = \
 -isystem $(DEPSDIR)/carb_sdk_plugins/include \
 -isystem $(DEPSDIR)/omni_connect_sdk/$(CONFIG)/include \
 -isystem $(DEPSDIR)/usd/$(CONFIG)/include \
 -isystem $(DEPSDIR)/usd/$(CONFIG)/include/$(BOOSTVER)

# USD libs (most of these not required, but this is a proper set for a fully featured Connector)
USD_LIBS = \
 -lboost_python310 \
 -lusd_ar \
 -lusd_arch \
 -lusd_gf \
 -lusd_js \
 -lusd_kind \
 -lusd_pcp \
 -lusd_plug \
 -lusd_sdf \
 -lusd_tf \
 -lusd_trace \
 -lusd_usd \
 -lusd_usdGeom \
 -lusd_usdLux \
 -lusd_usdShade \
 -lusd_usdUtils \
 -lusd_vt \
 -lusd_work

OMNI_LIBS = \
 -lcarb \
 -lomni_connect_core \
 -lomni_transcoding \
 -lomni_usd_resolver \
 -lomniclient

# Library dependency directories
 OMNI_USD_LIB_DIRS = \
 -L$(DEPSDIR)/carb_sdk_plugins/_build/linux-x86_64/$(CONFIG) \
 -L$(DEPSDIR)/omni_connect_sdk/$(CONFIG)/lib \
 -L$(DEPSDIR)/omni_transcoding/$(CONFIG)/lib \
 -L$(DEPSDIR)/omni_usd_resolver/$(CONFIG) \
 -L$(DEPSDIR)/omni_client_library/$(CONFIG) \
 -L$(DEPSDIR)/usd/$(CONFIG)/lib

# Python specifics
ifndef PYTHON_INCLUDE_DIR
	PYTHON_INCLUDE_DIR = -isystem $(DEPSDIR)/python/include/$(PYTHONVER)
endif

ifndef PYTHON_LIB
	PYTHON_LIB = -l$(PYTHONVER)
endif

ifndef PYTHON_LIB_DIR
	PYTHON_LIB_DIR = -L$(DEPSDIR)/python/lib
endif

# Common flags
CXXFLAGS += $(CONFIG_DEFINES) $(ABI_DEFINES) $(IGNORED_WARNINGS) -m64 -DTBB_SUPPRESS_DEPRECATED_MESSAGES
INCLUDES += $(OMNI_USD_INCLUDE_DIRS) $(PYTHON_INCLUDE_DIR)
LIBS += $(USD_LIBS) $(OMNI_LIBS) $(PYTHON_LIB)
LDFLAGS += $(OMNI_USD_LIB_DIRS) $(PYTHON_LIB_DIR)

OBJS = $(TARGETDIR)/$(PROGRAMNAME).o

# Build Targets

all: $(TARGETDIR)/$(PROGRAMNAME)

# $@ matches the target; $< matches the first dependent
$(TARGETDIR)/$(PROGRAMNAME): $(OBJS)
	echo Linking $(PROGRAMNAME)
	g++ -o $@ $< $(LDFLAGS) $(LIBS)

$(OBJS): $(PROGRAMNAME).cpp | $(TARGETDIR)
	g++ $(INCLUDES) $(CXXFLAGS) -c $< -o $@

$(TARGETDIR):
	@echo Creating $(TARGETDIR)
	@mkdir -p $(TARGETDIR)

clean:
	rm -rf $(TARGETDIR)
