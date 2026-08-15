#pragma once
// AYShader/detail/AYShader/detail/AYShader/detail/AYShader/detail/PhoskiaFrameBuiltins.h — frame builtins (Phoskia language surface)
//
// Per-draw / per-view state from bgfx common.sh. Available in vertex { }
// and fragment { } blocks. The BGFX backend lowers these to u_* names
// during .sc emission — Phoskia source must not reference bgfx directly.

#include "AYShader/Type.h"

#include <cstddef>
#include <string>

namespace ayt::shader::phoskia::detail
{

enum class FrameBuiltinKind {
    Mat4,
    Vec4,
    Float,
};

struct FrameBuiltinSpec {
    const char* phoskiaName;
    const char* bgfxExpr;
    FrameBuiltinKind kind;
};

inline constexpr FrameBuiltinSpec kFrameBuiltins[] = {
    {"modelViewProjection",         "u_modelViewProj",  FrameBuiltinKind::Mat4},
    {"modelMatrix",                 "u_model[0]",       FrameBuiltinKind::Mat4},
    {"modelViewMatrix",             "u_modelView",      FrameBuiltinKind::Mat4},
    {"inverseModelViewMatrix",      "u_invModelView",   FrameBuiltinKind::Mat4},
    {"viewMatrix",                  "u_view",           FrameBuiltinKind::Mat4},
    {"inverseViewMatrix",           "u_invView",        FrameBuiltinKind::Mat4},
    {"projectionMatrix",            "u_proj",           FrameBuiltinKind::Mat4},
    {"inverseProjectionMatrix",     "u_invProj",        FrameBuiltinKind::Mat4},
    {"viewProjectionMatrix",        "u_viewProj",       FrameBuiltinKind::Mat4},
    {"inverseViewProjectionMatrix", "u_invViewProj",    FrameBuiltinKind::Mat4},
    {"viewportRect",                "u_viewRect",       FrameBuiltinKind::Vec4},
    {"viewportTexel",               "u_viewTexel",      FrameBuiltinKind::Vec4},
    {"alphaReference",              "u_alphaRef",       FrameBuiltinKind::Float},
};

inline constexpr std::size_t kFrameBuiltinCount =
    sizeof(kFrameBuiltins) / sizeof(kFrameBuiltins[0]);

inline const FrameBuiltinSpec* findFrameBuiltin(const std::string& name)
{
    for (std::size_t i = 0; i < kFrameBuiltinCount; ++i) {
        if (name == kFrameBuiltins[i].phoskiaName) {
            return &kFrameBuiltins[i];
        }
    }
    return nullptr;
}

inline void registerFrameBuiltins(TypeEnvironment& env)
{
    for (std::size_t i = 0; i < kFrameBuiltinCount; ++i) {
        const FrameBuiltinSpec& spec = kFrameBuiltins[i];
        switch (spec.kind) {
        case FrameBuiltinKind::Mat4:
            env.addVariable(spec.phoskiaName, BuiltinTypes::Mat4());
            break;
        case FrameBuiltinKind::Vec4:
            env.addVariable(spec.phoskiaName, BuiltinTypes::Vec4());
            break;
        case FrameBuiltinKind::Float:
            env.addVariable(spec.phoskiaName, BuiltinTypes::Float);
            break;
        }
    }
}

// Backward-compatible alias.
inline void registerVertexFrameBuiltins(TypeEnvironment& env)
{
    registerFrameBuiltins(env);
}

inline bool isFrameBuiltin(const std::string& name)
{
    return findFrameBuiltin(name) != nullptr;
}

inline bool isVertexFrameBuiltin(const std::string& name)
{
    return isFrameBuiltin(name);
}

inline bool frameBuiltinIsMatrix(const std::string& name)
{
    const FrameBuiltinSpec* spec = findFrameBuiltin(name);
    return spec != nullptr && spec->kind == FrameBuiltinKind::Mat4;
}

inline const char* bgfxFrameBuiltinExpr(const std::string& name)
{
    const FrameBuiltinSpec* spec = findFrameBuiltin(name);
    return spec != nullptr ? spec->bgfxExpr : name.c_str();
}

} // namespace ayt::shader::phoskia::detail
