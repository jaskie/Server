#pragma once

// NOTE : The following license applies to this file ONLY and not to the SDK as a whole. Please review the SDK documentation for
// the description of the full license terms.
//
//***********************************************************************************************************************************************
// 
// Copyright(c) 2016 NewTek, inc
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation 
// files(the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, 
// merge, publish, distribute, sublicense, and / or sell copies of the Software, and to permit persons to whom the Software is 
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE 
// FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION 
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
//***********************************************************************************************************************************************

// Is this library being compiled, or imported by another application.
#ifdef _WIN32
#define PROCESSINGNDILIB_DEPRECATED __declspec(deprecated)
#ifdef PROCESSINGNDILIB_EXPORTS
#ifdef __cplusplus
#define PROCESSINGNDILIB_API extern "C" __declspec(dllexport)
#else
#define PROCESSINGNDILIB_API __declspec(dllexport)
#endif
#else
#ifdef __cplusplus
#define PROCESSINGNDILIB_API extern "C" __declspec(dllimport)
#else
#define PROCESSINGNDILIB_API __declspec(dllimport)
#endif
#ifdef _MSC_VER
#ifdef _WIN64
#pragma comment(lib, "Processing.NDI.Lib.x64.lib")
#else
#pragma comment(lib, "Processing.NDI.Lib.x86.lib")
#endif
#endif
#endif
#else
#define PROCESSINGNDILIB_DEPRECATED
#ifdef __cplusplus
#define PROCESSINGNDILIB_API extern "C" __attribute((visibility("default")))
#else
#define PROCESSINGNDILIB_API __attribute((visibility("default")))
#endif
#endif

// Data structures shared by multiple SDKs
#include "Processing.NDI.compat.h"
#include "Processing.NDI.structs.h"

// This is not actually required, but will start and end the libraries which might get
// you slightly better performance in some cases. In general it is more "correct" to 
// call these although it is not required. There is no way to call these that would have
// an adverse impact on anything (even calling destroy before you've deleted all your
// objects). This will return false if the CPU is not sufficiently capable to run NDILib
// currently NDILib requires SSE4.2 instructions (see documentation). You can verify 
// a specific CPU against the library with a call to NDIlib_is_supported_CPU()
PROCESSINGNDILIB_API
bool NDIlib_initialize(void);

PROCESSINGNDILIB_API
void NDIlib_destroy(void);

PROCESSINGNDILIB_API
const char* NDIlib_version(void);

// Recover whether the current CPU in the system is capable of running NDILib.
PROCESSINGNDILIB_API
bool NDIlib_is_supported_CPU(void);

// The main SDKs
#include "Processing.NDI.Find.h"
#include "Processing.NDI.Recv.h"
#include "Processing.NDI.Send.h"
#include "Processing.NDI.Routing.h"

// Utility functions
#include "Processing.NDI.utilities.h"

// Dynamic loading used for OSS libraries
#include "Processing.NDI.DynamicLoad.h"
