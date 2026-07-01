// ============================================================
// AYShader PBR Function Library Tests (Phase 2 Step 3)
// ============================================================
//
// Exercises the five PBR building blocks registered in
// BuiltinFunctionRegistry::registerDefaults:
//
//   fresnelSchlick            (float cosTheta, vec3 F0)            -> vec3
//   fresnelSchlickRoughness   (float cosTheta, vec3 F0, float r)   -> vec3
//   distributionGGX           (float NdotH, float roughness)       -> float
//   geometrySchlickGGX        (float NdotV, float roughness)       -> float
//   geometrySmith             (float NdotV, float NdotL, float r)  -> float
//
// The runtime impls in AYBuiltinFunctions.cpp execute when invoked
// directly; the type checker / BGFX backend rely on the registry's
// paramTypes / returnType signatures only.

#include "AYBuiltinFunctions.h"
#include "AYType.h"
#include "AYTest.h"

#include <cmath>
#include <vector>
#include <variant>
#include <cstdio>

using namespace ayt::shader::phoskia;

namespace {

// Type-arg values: floats (most PBR params are floats).
using V = std::variant<std::monostate, bool, float, int, std::string>;
using Args = std::vector<V>;

float callFloat(const std::string& name, const Args& args) {
    auto* f = BuiltinFunctionRegistry::instance().getFunctionByArity(name, args.size());
    if (!f) return -1.0f;
    auto result = f->implementation(args);
    return std::get<float>(result);
}

// Convenience: float overload only (PBR tests don't need vec results).
inline V fv(float x) { return V{x}; }

}  // namespace

TEST_SUITE(PBRFunctionsTests)

// ===== fresnelSchlick =====

TEST_CASE(fresnelSchlick_known_in_registry) {
    auto* f = BuiltinFunctionRegistry::instance().getFunctionByArity("fresnelSchlick", 2);
    CHECK(f != nullptr);
    CHECK(f->paramTypes.size() == 2);
    CHECK(f->paramTypes[0]->equals(*BuiltinTypes::Float));
    auto v3 = BuiltinTypes::Vec3();
    CHECK(f->paramTypes[1]->equals(*v3));
    CHECK(f->returnType->equals(*v3));
}

TEST_CASE(fresnelSchlick_at_normal_incidence_equals_F0) {
    // cosTheta = 1.0 â†?(1-cosTheta)^5 = 0 â†?F = F0.
    // Runtime uses simplified args[1] as a scalar float proxy for F0.
    float result = callFloat("fresnelSchlick", {fv(1.0f), fv(0.04f)});
    CHECK_FLOAT_EQ(result, 0.04f, 1e-5f);
}

TEST_CASE(fresnelSchlick_at_grazing_equals_1) {
    // cosTheta = 0 â†?(1-0)^5 = 1 â†?F = F0 + (1-F0) = 1.
    float result = callFloat("fresnelSchlick", {fv(0.0f), fv(0.04f)});
    CHECK_FLOAT_EQ(result, 1.0f, 1e-5f);
}

TEST_CASE(fresnelSchlick_at_45deg_in_range) {
    // cosTheta â‰?0.707 â†?(1-0.707)^5 â‰?0.0007 â†?F â‰?F0 + (1-F0)*0.0007.
    float result = callFloat("fresnelSchlick", {fv(0.707f), fv(0.04f)});
    CHECK(result >= 0.04f);   // not below F0
    CHECK(result <= 1.0f);   // not above 1
    CHECK(result < 0.5f);    // 45 deg shouldn't be near full reflection
}

// ===== fresnelSchlickRoughness =====

TEST_CASE(fresnelSchlickRoughness_known_in_registry) {
    auto* f = BuiltinFunctionRegistry::instance().getFunctionByArity("fresnelSchlickRoughness", 3);
    CHECK(f != nullptr);
    CHECK(f->paramTypes.size() == 3);
    auto v3 = BuiltinTypes::Vec3();
    CHECK(f->returnType->equals(*v3));
}

TEST_CASE(fresnelSchlickRoughness_high_roughness_pushes_F_up) {
    // At grazing angle + high roughness, F should be much higher than the
    // non-rough Schlick variant (roughness^2 kicks in as the max).
    float plain = callFloat("fresnelSchlick", {fv(0.0f), fv(0.04f)});
    float rough = callFloat("fresnelSchlickRoughness", {fv(0.0f), fv(0.04f), fv(0.9f)});
    CHECK(rough >= plain);  // roughness path saturates to 1 at grazing
}

// ===== distributionGGX =====

TEST_CASE(distributionGGX_known_in_registry) {
    auto* f = BuiltinFunctionRegistry::instance().getFunctionByArity("distributionGGX", 2);
    CHECK(f != nullptr);
    CHECK(f->returnType->equals(*BuiltinTypes::Float));
}

TEST_CASE(distributionGGX_at_zero_NdotH) {
    // NdotH = 0 (H perpendicular to surface normal). Should give a small
    // but positive value depending on roughness â€?never negative.
    float result = callFloat("distributionGGX", {fv(0.0f), fv(0.5f)});
    CHECK(result >= 0.0f);
    CHECK(std::isfinite(result));
}

TEST_CASE(distributionGGX_at_aligned_NdotH) {
    // NdotH = 1 (H aligned with N). Should give a sharp peak.
    float result = callFloat("distributionGGX", {fv(1.0f), fv(0.5f)});
    CHECK(result > 0.0f);
    CHECK(std::isfinite(result));
}

TEST_CASE(distributionGGX_smooth_surface_sharper_at_peak) {
    // At NdotH=1 (perfect alignment with normal), a smooth surface
    // (roughness=0.1) produces a strong narrow peak, while a rough
    // surface (roughness=0.9) spreads the same energy over a wider
    // solid angle â€?so the value at NdotH=1 is lower.
    float smooth = callFloat("distributionGGX", {fv(1.0f), fv(0.1f)});
    float rough  = callFloat("distributionGGX", {fv(1.0f), fv(0.9f)});
    CHECK(smooth > rough);
}

// ===== geometrySchlickGGX =====

TEST_CASE(geometrySchlickGGX_known_in_registry) {
    auto* f = BuiltinFunctionRegistry::instance().getFunctionByArity("geometrySchlickGGX", 2);
    CHECK(f != nullptr);
    CHECK(f->returnType->equals(*BuiltinTypes::Float));
}

TEST_CASE(geometrySchlickGGX_at_grazing_view_clamps_to_zero) {
    // NdotV â†?0 â†?geometry term â†?0 (Schlick-GGX clamp).
    float result = callFloat("geometrySchlickGGX", {fv(0.0f), fv(0.5f)});
    CHECK_FLOAT_EQ(result, 0.0f, 1e-5f);
}

TEST_CASE(geometrySchlickGGX_at_perpendicular_view_is_one) {
    // NdotV = 1 â†?geometry term = 1 / (1*(1-k) + k) = 1.
    float result = callFloat("geometrySchlickGGX", {fv(1.0f), fv(0.5f)});
    CHECK_FLOAT_EQ(result, 1.0f, 1e-5f);
}

// ===== geometrySmith =====

TEST_CASE(geometrySmith_known_in_registry) {
    auto* f = BuiltinFunctionRegistry::instance().getFunctionByArity("geometrySmith", 3);
    CHECK(f != nullptr);
    CHECK(f->returnType->equals(*BuiltinTypes::Float));
}

TEST_CASE(geometrySmith_at_grazing_view_or_light_clamps_to_zero) {
    // G(0, *, r) = 0 because G_sub(0) = 0.
    float r1 = callFloat("geometrySmith", {fv(0.0f), fv(0.7f), fv(0.5f)});
    CHECK_FLOAT_EQ(r1, 0.0f, 1e-5f);
    float r2 = callFloat("geometrySmith", {fv(0.7f), fv(0.0f), fv(0.5f)});
    CHECK_FLOAT_EQ(r2, 0.0f, 1e-5f);
}

TEST_CASE(geometrySmith_at_perpendicular_equals_one) {
    // G(1, 1, r) = G_sub(1) * G_sub(1) = 1 * 1 = 1.
    float result = callFloat("geometrySmith", {fv(1.0f), fv(1.0f), fv(0.5f)});
    CHECK_FLOAT_EQ(result, 1.0f, 1e-5f);
}

TEST_CASE(geometrySmith_decreases_with_roughness) {
    // At fixed NdotV/NdotL, higher roughness â†?lower geometry term.
    float smooth = callFloat("geometrySmith", {fv(0.5f), fv(0.5f), fv(0.1f)});
    float rough  = callFloat("geometrySmith", {fv(0.5f), fv(0.5f), fv(0.9f)});
    CHECK(smooth >= rough);
}

// ===== Type-inference signature roundtrip =====

TEST_CASE(pbr_functions_appear_in_all_function_names) {
    auto names = BuiltinFunctionRegistry::instance().getAllFunctionNames();
    bool hasFresnel = false, hasGGX = false, hasSmith = false;
    for (const auto& n : names) {
        if (n == "fresnelSchlick") hasFresnel = true;
        if (n == "distributionGGX") hasGGX = true;
        if (n == "geometrySmith") hasSmith = true;
    }
    CHECK(hasFresnel);
    CHECK(hasGGX);
    CHECK(hasSmith);
}

TEST_SUITE_END