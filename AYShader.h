#pragma once
// AYShader.h - AYShader frontend entry (Phase 4-G)
//
// Include this from frontend / gameplay code that drives shaders through
// ShaderResource + ShaderResourcePool. Does NOT pull bgfx or legacy
// ShaderProgram handles.
//
// Internal tooling that needs BGFXConverter or disk ShaderCache should
// include AYShader/BGFXConverter.h / AYShader/ShaderCache.h directly.

#include "AYShader/ShaderProgram.h"
#include "AYShader/ShaderResource.h"
#include "AYShader/ShaderResourcePool.h"
#include "AYShader/IBackendConverter.h"

#include "AYShader/Phoskia.h"
