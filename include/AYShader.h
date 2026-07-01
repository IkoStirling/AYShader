#pragma once
// AYShader.h - AYShader main entry
//
// AYShader is the shader subsystem of AY Engine.
// It compiles Phoskia sources (a shader DSL) into platform-specific shader
// binaries (currently BGFX .sc via shaderc).
//
// Module layout:
//   ayt::shader             - Engine integration (Shader, ShaderProgram, ShaderCache)
//   ayt::shader::phoskia    - Phoskia compiler core (Lexer, Parser, AST, ...)

#include "AYShaderProgram.h"
#include "AYShaderResource.h"
#include "AYShaderResourcePool.h"
#include "AYShaderCache.h"
#include "IAYBackendConverter.h"
#include "AYBGFXConverter.h"

#include "AYPhoskia.h"
