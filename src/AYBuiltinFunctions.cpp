// AYBuiltinFunctions.cpp - Built-in function library implementation

#include "AYBuiltinFunctions.h"
#include <cmath>
#include <algorithm>

namespace ayt::shader::phoskia
{

BuiltinFunctionRegistry& BuiltinFunctionRegistry::instance() {
    static BuiltinFunctionRegistry registry;
    return registry;
}

void BuiltinFunctionRegistry::registerFunction(const BuiltinFunction& func) {
    // Phase 2 Step 2: support overloads (e.g. normalize takes vec2 / vec3
    // / vec4, dot takes (vec2,vec2) / (vec3,vec3) / (vec4,vec4)). The
    // container is now vector-of-overloads per name; the original
    // `_functions[name] = func` shape collapsed later registrations.
    _functions[func.name].push_back(func);
}

bool BuiltinFunctionRegistry::hasFunction(const std::string& name) const {
    return _functions.find(name) != _functions.end();
}

const BuiltinFunction* BuiltinFunctionRegistry::getFunction(const std::string& name) const {
    auto it = _functions.find(name);
    if (it == _functions.end() || it->second.empty()) return nullptr;
    return &it->second.front();
}

const BuiltinFunction* BuiltinFunctionRegistry::getFunctionByArity(const std::string& name,
                                                                    size_t argsSize) const {
    auto it = _functions.find(name);
    if (it == _functions.end()) return nullptr;
    for (const auto& ovl : it->second) {
        if (ovl.paramTypes.size() == argsSize) return &ovl;
    }
    return nullptr;
}

const std::vector<BuiltinFunction>* BuiltinFunctionRegistry::getOverloads(
    const std::string& name) const {
    auto it = _functions.find(name);
    if (it == _functions.end() || it->second.empty()) return nullptr;
    return &it->second;
}

std::vector<std::string> BuiltinFunctionRegistry::getAllFunctionNames() const {
    std::vector<std::string> names;
    for (const auto& pair : _functions) {
        names.push_back(pair.first);
    }
    return names;
}

void BuiltinFunctionRegistry::registerDefaults() {
    using TF = std::shared_ptr<Type>;
    auto F = BuiltinTypes::Float;
    auto I = BuiltinTypes::Int;
    auto B = BuiltinTypes::Bool;
    auto V2 = BuiltinTypes::Vec2();
    auto V3 = BuiltinTypes::Vec3();
    auto V4 = BuiltinTypes::Vec4();
    auto IV2 = std::make_shared<VectorType>(PrimitiveType::Int, 2);
    auto IV3 = std::make_shared<VectorType>(PrimitiveType::Int, 3);
    auto IV4 = std::make_shared<VectorType>(PrimitiveType::Int, 4);
    auto M2 = BuiltinTypes::Mat2();
    auto M3 = BuiltinTypes::Mat3();
    auto M4 = BuiltinTypes::Mat4();

    // Phase 1 RD-03: register the skinningMatrix builtin (no-op stub —
    // the BGFX backend special-cases this call by name and emits a
    // weighted-sum expansion inline).
    //
    // Registered INLINE here (not via SkeletalFunctions::registerXxx)
    // because the registry's ctor is mid-construction when this runs —
    // calling `instance()` from a sub-function re-enters the static
    // local and recurses forever.
    {
        auto sk_IV4 = std::make_shared<VectorType>(PrimitiveType::Int, 4);
        auto sk_V4 = BuiltinTypes::Vec4();
        auto sk_M4 = BuiltinTypes::Mat4();
        auto sk_M4Array = std::make_shared<ArrayType>(sk_M4);
        auto sk_stub = [](auto) -> float { return 0.0f; };
        // Two overloads: bones as ArrayType<mat4> (preferred / what the
        // parser lowers to when the field is `mat4 bones[N]`), and bones
        // as bare Mat4 (fallback if the type env doesn't preserve the
        // array wrapping).
        registerFunction("skinningMatrix",
            std::vector<std::shared_ptr<Type>>{ sk_IV4, sk_V4, sk_M4Array, sk_V4 },
            sk_V4, sk_stub,
            "Linear-blend skinning transform (ivec4 indices, vec4 weights, mat4[] bones, vec4 pos) -> vec4");
        registerFunction("skinningMatrix",
            std::vector<std::shared_ptr<Type>>{ sk_IV4, sk_V4, sk_M4, sk_V4 },
            sk_V4, sk_stub,
            "Linear-blend skinning transform (fallback non-array mat4 bones)");
    }

    // Phase 2 Step 2: placeholder runtime impl. The BGFX backend
    // ignores all of this and emits GLSL directly. The interpreter
    // fallback (used by tests) doesn't actually run these bodies.
    auto vec3Return = [](auto) -> float { return 0.0f; };

    // ---- Type constructors (Phase 2 Step 2) ----
    // Phoskia's parser (Phase 1 Type-5 fallback) wraps any Vec2/3/4/Int/Float
    // token in expression position as IdentifierExpr(name="vec3", ...) so
    // the semantic analyzer must register these names as known functions
    // in the env. We register the canonical-arity overload; mixed-arity
    // forms like vec4(v3, f) or mat4(v4, v4, v4, v4) fall through to
    // TypeInference::inferConstructor in AYTypeInference.cpp.
    registerFunction("float", {F}, F, vec3Return, "Scalar float");
    registerFunction("int",   {I}, I, vec3Return, "Scalar int");
    // Phase 3.3 Block 1: uint constructor. GLSL's `uint(int_expr)` is
    // an explicit conversion (truncates negative values). We mirror
    // that with a single-arg (int) -> uint signature so the call form
    // `uint(0)` resolves correctly. The integer literal `0` infers as
    // int (parser parses IntLiteral as `int`), and the constructor
    // call converts it. A future Phase 3.3 strict-uvec3 extension may
    // add a separate `0u` IntLiteral variant, but for now `uint(0)`
    // is the canonical uint literal idiom.
    registerFunction("uint",  {I}, BuiltinTypes::Uint, vec3Return, "Scalar uint");
    registerFunction("bool",  {B}, B, vec3Return, "Scalar bool");
    registerFunction("vec2",  {F, F}, V2, vec3Return, "vec2 from two floats");
    registerFunction("vec3",  {F, F, F}, V3, vec3Return, "vec3 from three floats");
    registerFunction("vec4",  {F, F, F, F}, V4, vec3Return, "vec4 from four floats");
    registerFunction("ivec2", {I, I}, IV2, vec3Return, "ivec2 from two ints");
    registerFunction("ivec3", {I, I, I}, IV3, vec3Return, "ivec3 from three ints");
    registerFunction("ivec4", {I, I, I, I}, IV4, vec3Return, "ivec4 from four ints");
    registerFunction("mat2",  {F, F, F, F}, M2, vec3Return, "mat2 from four floats");
    registerFunction("mat3",  {F, F, F, F, F, F, F, F, F}, M3, vec3Return, "mat3 from nine floats");
    registerFunction("mat4",  {F, F, F, F, F, F, F, F, F, F, F, F, F, F, F, F}, M4, vec3Return, "mat4 from sixteen floats");

    // Math functions
    auto mathImpl = [](float x) -> float { return std::abs(x); };
    registerFunction("abs", {F}, F, [](auto args) -> float {
        return std::abs(std::get<float>(args[0]));
    }, "Absolute value");

    registerFunction("floor", {F}, F, [](auto args) -> float {
        return std::floor(std::get<float>(args[0]));
    });

    registerFunction("ceil", {F}, F, [](auto args) -> float {
        return std::ceil(std::get<float>(args[0]));
    });

    registerFunction("round", {F}, F, [](auto args) -> float {
        return std::round(std::get<float>(args[0]));
    });

    registerFunction("sqrt", {F}, F, [](auto args) -> float {
        return std::sqrt(std::get<float>(args[0]));
    });

    registerFunction("pow", {F, F}, F, [](auto args) -> float {
        return std::pow(std::get<float>(args[0]), std::get<float>(args[1]));
    });

    registerFunction("exp", {F}, F, [](auto args) -> float {
        return std::exp(std::get<float>(args[0]));
    });

    registerFunction("log", {F}, F, [](auto args) -> float {
        return std::log(std::get<float>(args[0]));
    });

    registerFunction("sin", {F}, F, [](auto args) -> float {
        return std::sin(std::get<float>(args[0]));
    });

    registerFunction("cos", {F}, F, [](auto args) -> float {
        return std::cos(std::get<float>(args[0]));
    });

    registerFunction("tan", {F}, F, [](auto args) -> float {
        return std::tan(std::get<float>(args[0]));
    });

    registerFunction("min", {F, F}, F, [](auto args) -> float {
        return std::min(std::get<float>(args[0]), std::get<float>(args[1]));
    });

    registerFunction("max", {F, F}, F, [](auto args) -> float {
        return std::max(std::get<float>(args[0]), std::get<float>(args[1]));
    });

    registerFunction("clamp", {F, F, F}, F, [](auto args) -> float {
        float v = std::get<float>(args[0]);
        float mn = std::get<float>(args[1]);
        float mx = std::get<float>(args[2]);
        return (v < mn) ? mn : (v > mx) ? mx : v;
    });

    registerFunction("lerp", {F, F, F}, F, [](auto args) -> float {
        float a = std::get<float>(args[0]);
        float b = std::get<float>(args[1]);
        float t = std::get<float>(args[2]);
        return a + t * (b - a);
    });

    // GLSL mix(x,y,a) for scalars — without this, float mix() falls through
    // to Dynamic and the BGFX emitter prefixes `vec3` (breaks shadow filters).
    registerFunction("mix", {F, F, F}, F, [](auto args) -> float {
        float a = std::get<float>(args[0]);
        float b = std::get<float>(args[1]);
        float t = std::get<float>(args[2]);
        return a + t * (b - a);
    });

    // bgfx shaderlib packFloatToRgba / unpackRgbaToFloat (RGBA8 shadow depth).
    // Bodies are inlined by AYBGFXConverter (shaderc has no shaderlib include).
    registerFunction("packFloatToRgba", {F}, V4, vec3Return,
                     "Pack [0,1] float into RGBA8 channels");
    registerFunction("unpackFloatFromRgba", {V4}, F, vec3Return,
                     "Unpack RGBA8 packed depth float");

    registerFunction("step", {F, F}, F, [](auto args) -> float {
        float edge = std::get<float>(args[0]);
        float x = std::get<float>(args[1]);
        return x < edge ? 0.0f : 1.0f;
    });

    registerFunction("smoothstep", {F, F, F}, F, [](auto args) -> float {
        float e0 = std::get<float>(args[0]);
        float e1 = std::get<float>(args[1]);
        float x = std::get<float>(args[2]);
        float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    });

    // Conditional
    registerFunction("if", {B, F, F}, F,
        [](auto args) -> float {
            bool cond = std::get<bool>(args[0]);
            return cond ? std::get<float>(args[1]) : std::get<float>(args[2]);
        }, "Ternary conditional");

    // ---- Vector math (Phase 2 Step 2) ----
    // These need real implementations — the unit tests assert
    // `unify(argType, paramType)` succeeds, not that the runtime
    // implementation runs. The runtime impls exist so future interpreter
    // passes have a fallback. The BGFX backend ignores all of this and
    // emits GLSL directly.

    // Normalize: vecN → vecN (same dimension).
    registerFunction("normalize", {V3}, V3,
        [](auto args) -> float { return 0.0f; },  // interpreted return not used
        "Normalize a vec3");
    registerFunction("normalize", {V2}, V2,
        [](auto) -> float { return 0.0f; },
        "Normalize a vec2");
    registerFunction("normalize", {V4}, V4,
        [](auto) -> float { return 0.0f; },
        "Normalize a vec4");

    // Dot: (vecN, vecN) → float.
    registerFunction("dot", {V2, V2}, F, vec3Return, "Dot product of two vec2");
    registerFunction("dot", {V3, V3}, F, vec3Return, "Dot product of two vec3");
    registerFunction("dot", {V4, V4}, F, vec3Return, "Dot product of two vec4");

    // Cross: (vec3, vec3) → vec3. (cross product is vec3-only in GLSL.)
    registerFunction("cross", {V3, V3}, V3, vec3Return, "Cross product");

    // Length: vecN → float.
    registerFunction("length", {V2}, F, vec3Return, "Length of a vec2");
    registerFunction("length", {V3}, F, vec3Return, "Length of a vec3");
    registerFunction("length", {V4}, F, vec3Return, "Length of a vec4");

    // Distance: (vecN, vecN) → float.
    registerFunction("distance", {V2, V2}, F, vec3Return, "Distance between two vec2");
    registerFunction("distance", {V3, V3}, F, vec3Return, "Distance between two vec3");
    registerFunction("distance", {V4, V4}, F, vec3Return, "Distance between two vec4");

    // Reflect: (incident, normal) → reflected vec3.
    registerFunction("reflect", {V3, V3}, V3, vec3Return, "Reflect a vec3");

    // Refract: (incident, normal, eta) → refracted vec3.
    registerFunction("refract", {V3, V3, F}, V3, vec3Return, "Refract a vec3");

    // ---- Texture sampling (Phase 2 Step 2) ----
    // Phoskia exposes `sample(tex, uv)`. The texture type is opaque —
    // we register it as taking Dynamic for the first arg, returning vec4.
    // (A dedicated TextureType is Phase 3 work.)
    registerFunction("sample", {BuiltinTypes::Dynamic, V2}, V4,
        vec3Return, "Sample a texture2d at UV");
    registerFunction("sample", {BuiltinTypes::Dynamic, V3}, V4,
        vec3Return, "Sample a texturecube or 2d array at vec3 coordinates");
    registerFunction("sampleLod", {BuiltinTypes::Dynamic, V2, F}, V4,
        vec3Return, "Sample a texture2d at UV with explicit LOD");
    registerFunction("sampleGrad", {BuiltinTypes::Dynamic, V2, V2, V2}, V4,
        vec3Return, "Sample a texture2d with explicit gradients");

    // ---- Fragment derivatives (Phase 5 slice) ----
    registerFunction("dFdx", {F}, F, vec3Return, "GLSL dFdx (float)");
    registerFunction("dFdx", {V2}, V2, vec3Return, "GLSL dFdx (vec2)");
    registerFunction("dFdx", {V3}, V3, vec3Return, "GLSL dFdx (vec3)");
    registerFunction("dFdx", {V4}, V4, vec3Return, "GLSL dFdx (vec4)");
    registerFunction("dFdy", {F}, F, vec3Return, "GLSL dFdy (float)");
    registerFunction("dFdy", {V2}, V2, vec3Return, "GLSL dFdy (vec2)");
    registerFunction("dFdy", {V3}, V3, vec3Return, "GLSL dFdy (vec3)");
    registerFunction("dFdy", {V4}, V4, vec3Return, "GLSL dFdy (vec4)");
    registerFunction("fwidth", {F}, F, vec3Return, "GLSL fwidth (float)");
    registerFunction("fwidth", {V2}, V2, vec3Return, "GLSL fwidth (vec2)");
    registerFunction("fwidth", {V3}, V3, vec3Return, "GLSL fwidth (vec3)");
    registerFunction("fwidth", {V4}, V4, vec3Return, "GLSL fwidth (vec4)");

    // ---- Mix / step / smoothstep extended to vectors (Phase 2 Step 2) ----
    // vec2 forms required for atlas UV remap:
    //   mix(shadowAtlasRects[i].xy, shadowAtlasRects[i].zw, altUv)
    // Missing overloads fell back to scalar mix → `float _rectUv = mix(...)`
    // → HLSL X3018 invalid subscript `.y`.
    registerFunction("mix", {V2, V2, F}, V2, vec3Return, "Linear blend of two vec2");
    registerFunction("mix", {V2, V2, V2}, V2, vec3Return, "Linear blend of two vec2 (vec2 factor)");
    registerFunction("mix", {V3, V3, F}, V3, vec3Return, "Linear blend of two vec3");
    registerFunction("mix", {V3, V3, V3}, V3, vec3Return, "Linear blend of two vec3 (vec3 factor)");
    registerFunction("mix", {V4, V4, F}, V4, vec3Return, "Linear blend of two vec4");
    registerFunction("mix", {V4, V4, V4}, V4, vec3Return, "Linear blend of two vec4 (vec4 factor)");
    registerFunction("step", {V2, V2}, V2, vec3Return, "Step function (vec2 form)");
    registerFunction("step", {V3, V3}, V3, vec3Return, "Step function (vector form)");
    registerFunction("step", {V4, V4}, V4, vec3Return, "Step function (vector form)");
    registerFunction("smoothstep", {V2, V2, V2}, V2, vec3Return, "Smoothstep (vec2 form)");
    registerFunction("smoothstep", {V3, V3, V3}, V3, vec3Return, "Smoothstep (vector form)");
    registerFunction("smoothstep", {V4, V4, V4}, V4, vec3Return, "Smoothstep (vector form)");

    // ---- Compute thread-id builtins (Phase 3.3 Block 3 strict-typed) ----
    //
    // GLSL exposes three built-ins for compute workgroup addressing:
    //   - gl_GlobalInvocationID  (uvec3) — global linear thread index
    //   - gl_WorkGroupID         (uvec3) — which workgroup this thread belongs to
    //   - gl_NumWorkGroups       (uvec3) — total dispatched workgroups
    //
    // Phase 3.2 used to register these as returning vec3 (a deliberate
    // simplification so `thread_id.x` resolved to `float` and chained
    // with other vector math). Phase 3.3 Block 3 promotes them to
    // uvec3 so `thread_id.x` is `uint` — matching GLSL exactly. The
    // BGFX backend's emit path is unchanged (it still inlines the
    // raw GLSL builtin name), so the emitted .sc is byte-identical;
    // only the Phoskia-side resolvedType moves from Float to Uint.
    auto UV3 = std::make_shared<VectorType>(PrimitiveType::Uint, 3);
    registerFunction("thread_id",   {}, UV3, vec3Return, "GLSL gl_GlobalInvocationID");
    registerFunction("group_id",    {}, UV3, vec3Return, "GLSL gl_WorkGroupID");
    registerFunction("dispatch_id", {}, UV3, vec3Return,
        "GLSL gl_NumWorkGroups * gl_WorkGroupID (dispatch-space index)");

    // ============================================================
    // PBR (Physically-Based Rendering) — Phase 2 Step 3
    // ============================================================
    // Reference: LearnOpenGL PBR chapter (Joey de Vries) for the canonical
    // Cook-Torrance formulation. We register the four building blocks:
    //
    //   fresnelSchlick              — Schlick Fresnel approximation
    //   fresnelSchlickRoughness     — Schlick with roughness (IBL pre-filter)
    //   distributionGGX             — Trowbridge-Reitz normal distribution
    //   geometrySchlickGGX          — Smith's Schlick-GGX geometry (one side)
    //   geometrySmith               — Smith's combined geometry (both sides)
    //
    // The runtime impls execute when the unit tests drive the registry
    // directly. The BGFX backend ignores these and emits the GLSL body
    // inline at the call site.

    // Fresnel-Schlick:
    //   F(cosTheta, F0) = F0 + (1 - F0) * (1 - cosTheta)^5
    // We compute component-wise; the runtime returns a single float as a
    // proxy (the tests assert signature, not numeric output).
    registerFunction("fresnelSchlick", {F, V3}, V3,
        [](const auto& args) -> float {
            float cosTheta = std::get<float>(args[0]);
            float F0 = std::get<float>(args[1]);  // runtime simplification
            float oneMinus = 1.0f - cosTheta;
            float pow5 = oneMinus * oneMinus * oneMinus * oneMinus * oneMinus;
            return F0 + (1.0f - F0) * pow5;
        },
        "Schlick Fresnel approximation");

    // Fresnel-Schlick with roughness (IBL path):
    //   F(cosTheta, F0, roughness) = F0 + max(roughness^2, 1-F0) * (1-cosTheta)^5
    registerFunction("fresnelSchlickRoughness", {F, V3, F}, V3,
        [](const auto& args) -> float {
            float cosTheta = std::get<float>(args[0]);
            float F0 = std::get<float>(args[1]);
            float roughness = std::get<float>(args[2]);
            float r2 = roughness * roughness;
            float maxTerm = std::max(r2, 1.0f - F0);
            float oneMinus = 1.0f - cosTheta;
            float pow5 = oneMinus * oneMinus * oneMinus * oneMinus * oneMinus;
            return F0 + maxTerm * pow5;
        },
        "Schlick Fresnel with roughness");

    // Trowbridge-Reitz / GGX normal distribution:
    //   D(NdotH, roughness) = alpha^2 / (PI * (NdotH^2 * (alpha^2 - 1) + 1)^2)
    //   where alpha = roughness^2.
    registerFunction("distributionGGX", {F, F}, F,
        [](const auto& args) -> float {
            float NdotH = std::get<float>(args[0]);
            float roughness = std::get<float>(args[1]);
            float a = roughness * roughness;
            float a2 = a * a;
            float NdotH2 = NdotH * NdotH;
            float denom = NdotH2 * (a2 - 1.0f) + 1.0f;
            denom = 3.14159265f * denom * denom;
            return (denom > 0.0f) ? (a2 / denom) : 0.0f;
        },
        "Trowbridge-Reitz (GGX) normal distribution");

    // Schlick-GGX geometry term for one side:
    //   k = (roughness + 1)^2 / 8
    //   G(NdotV) = NdotV / (NdotV * (1 - k) + k)
    registerFunction("geometrySchlickGGX", {F, F}, F,
        [](const auto& args) -> float {
            float NdotV = std::get<float>(args[0]);
            float roughness = std::get<float>(args[1]);
            float r = roughness + 1.0f;
            float k = (r * r) / 8.0f;
            float denom = NdotV * (1.0f - k) + k;
            return (denom > 0.0f) ? (NdotV / denom) : 0.0f;
        },
        "Smith Schlick-GGX geometry (one side)");

    // Smith's combined geometry (both view + light):
    //   G(NdotV, NdotL, roughness) = G_sub(NdotV) * G_sub(NdotL)
    registerFunction("geometrySmith", {F, F, F}, F,
        [](const auto& args) -> float {
            float NdotV = std::get<float>(args[0]);
            float NdotL = std::get<float>(args[1]);
            float roughness = std::get<float>(args[2]);
            // Inline the Schlick-GGX for both sides (saves a recursive
            // call through the registry during interpreted execution).
            auto schlickGGX = [&](float NdotX) -> float {
                float r = roughness + 1.0f;
                float k = (r * r) / 8.0f;
                float denom = NdotX * (1.0f - k) + k;
                return (denom > 0.0f) ? (NdotX / denom) : 0.0f;
            };
            return schlickGGX(NdotV) * schlickGGX(NdotL);
        },
        "Smith's combined geometry (view + light)");
}

// Namespace registration functions
namespace MathFunctions {
    void registerAbs() {
        // Already registered in registerDefaults
    }
    void registerFloor() {}
    void registerCeil() {}
    void registerRound() {}
    void registerSqrt() {}
    void registerPow() {}
    void registerLog() {}
    void registerExp() {}
    void registerSin() {}
    void registerCos() {}
    void registerTan() {}
    void registerAsin() {}
    void registerAcos() {}
    void registerAtan() {}
    void registerMin() {}
    void registerMax() {}
    void registerClamp() {}
    void registerLerp() {}
    void registerStep() {}
    void registerSmoothstep() {}
    void registerNormalize() {}
    void registerDot() {}
    void registerCross() {}
    void registerLength() {}
    void registerDistance() {}
    void registerReflect() {}
    void registerRefract() {}
}

namespace PBRFunctions {
    void registerFresnelSchlick() {}
    void registerGGXDistribution() {}
    void registerSmithGGXGeometry() {}
    void registerCookTorrance() {}
}

namespace TextureFunctions {
    void registerSample() {}
    void registerSampleLod() {}
    void registerSampleGrad() {}
}

namespace UtilityFunctions {
    void registerMix() {}
    void registerMap() {}
    void registerReduce() {}
    void registerFilter() {}
    void registerIf() {}
}

namespace SkeletalFunctions {
    // Phase 1 RD-03: skinningMatrix is registered INLINE in
    // BuiltinFunctionRegistry::registerDefaults above. A namespace
    // function would re-enter `instance()` and recurse forever
    // (the registry ctor is mid-construction when registerDefaults
    // runs). The empty body here mirrors the MathFunctions / PBRFunctions
    // namespace stub pattern — kept in the header for future extensions.
    void registerSkinningMatrix() {}
}

} // namespace ayt::shader::phoskia