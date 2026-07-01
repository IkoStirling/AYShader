#pragma once
// AYShader.h - AYShader frontend entry (Phase 4-G)
//
// Include this from frontend / gameplay code that drives shaders through
// ShaderResource + ShaderResourcePool. Does NOT pull bgfx or legacy
// ShaderProgram handles.
//
// Internal tooling that needs BGFXConverter or disk ShaderCache should
// include AYBGFXConverter.h / AYShaderCache.h directly.

#include "AYShaderProgram.h"
#include "AYShaderResource.h"
#include "AYShaderResourcePool.h"
#include "IAYBackendConverter.h"

#include "AYPhoskia.h"
