// Single place that declares the REAPER API functions the extension uses.
//
// Exactly one translation unit (extension.cpp) defines REAPERAPI_IMPLEMENT
// before including this header; every other TU just sees the extern pointers.
#pragma once

#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_plugin_register
#define REAPERAPI_WANT_GetMainHwnd
#define REAPERAPI_WANT_GetResourcePath
#define REAPERAPI_WANT_GetProjectPath
#define REAPERAPI_WANT_GetProjectName
#define REAPERAPI_WANT_EnumProjects
#define REAPERAPI_WANT_IsProjectDirty
#define REAPERAPI_WANT_Main_openProject

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"
