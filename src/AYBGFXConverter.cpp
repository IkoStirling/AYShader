// AYBGFXConverter.cpp - BGFX backend converter implementation
//
// Converts a Phoskia material into three bgfx .sc files:
//   vs_<Material>.sc  — vertex shader source
//   fs_<Material>.sc  — fragment shader source
//   varying.def.sc    — shared varying/attribute semantic bindings
//
// These three files are then passed to bgfx's shaderc.exe (twice — once
// with --type vertex for vs_, once with --type fragment --varyingdef for
// fs_) to produce the binary shader programs bgfx::createProgram consumes.

#include "AYBGFXConverter.h"
#include "detail/AYPhoskiaFrameBuiltins.h"
#include "detail/AYShaderSourceKeys.h"
#include "detail/AYStd140Layout.h"
#include "AYAst.h"
#include "AYType.h"
#include "AYTypeInference.h"  // Phase 2: let stmt needs GLSL type prefix.

#include <AYFile.h>
#include <AYDirectory.h>
#include <AYPath.h>

#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace ayt::shader
{

namespace {

// Phase 2 Step 2 / Step 8: a per-conversion TypeEnvironment for the
// per-let type inference the converter performs when emitting GLSL.
// The env accumulates bindings as the converter walks the body in
// order: each `let x = ...` registers `x` with its inferred type so
// subsequent references (`let y = x * 2.0`) see a concrete type
// instead of a fresh TypeVar. Without this, a chain of arithmetic
// on bare identifiers (e.g. PBR pow5 = oneMinusNdotV * ... *
// oneMinusNdotV) would never resolve to any concrete type and the
// let-stmt emission would lack a GLSL type prefix.
//
// We thread the env through emitStmt as a parameter rather than a
// global so two consecutive convertBGFX calls (multiple materials,
// multiple shaders per material) do not leak bindings from one
// invocation into the next — that was the previous bug, where
// golden-fixture ordering could change let-type resolution.

// PhoskiaSemantic → bgfx attribute / varying names. Vertex attributes keep
// the usual POSITION/NORMAL/TEXCOORD0 bindings. Interpolated varyings use
// bgfx/shaderc dedicated semantics where available (NORMAL, COLOR0) and
// sequential TEXCOORDn for generic vec outputs — same pattern as
// thirdparty/bgfx/examples/44-sss and examples/49-hextile/varying.def.sc.
// shaderc maps these to the correct interpolator registers on D3D11/12, GL,
// Metal, Vulkan, etc.; the invariant we enforce is unique semantics per
// varying, not a platform-specific slot hack.
struct PhoskiaSemanticInfo {
    const char* attrSemantic;   // POSITION / NORMAL / TEXCOORD0 (vertex inputs)
    const char* attrName;       // a_position / a_normal / ...
    const char* varyingName;    // v_position / v_normal / ...
    const char* glslType;       // vec3 / vec4 / vec2
    const char* defaultExpr;    // vec3(0.0, 0.0, 0.0) etc.
};

struct VaryingLayoutPlan {
    bool hasNormalVarying    = false;
    bool hasTexcoordVarying  = false;
    bool hasColorVarying     = false;
    bool hasPositionVarying  = false;
};

VaryingLayoutPlan computeVaryingLayout(const phoskia::ir::IRVertexFunc* vf,
                                       const phoskia::ir::IRFragmentFunc* ff)
{
    VaryingLayoutPlan plan;
    const auto scan = [&](const std::vector<phoskia::ir::IRStmtPtr>& params,
                          phoskia::ir::IRShaderParam::Direction dir,
                          phoskia::PhoskiaSemantic sem,
                          bool& flag) {
        for (const auto& s : params) {
            const auto* p = dynamic_cast<const phoskia::ir::IRShaderParam*>(s.get());
            if (p != nullptr && p->dir == dir && p->semantic == sem) {
                flag = true;
            }
        }
    };

    if (vf != nullptr) {
        scan(vf->params, phoskia::ir::IRShaderParam::Direction::Out,
             phoskia::PhoskiaSemantic::Normal, plan.hasNormalVarying);
        scan(vf->params, phoskia::ir::IRShaderParam::Direction::Out,
             phoskia::PhoskiaSemantic::Texcoord, plan.hasTexcoordVarying);
        scan(vf->params, phoskia::ir::IRShaderParam::Direction::Out,
             phoskia::PhoskiaSemantic::Color, plan.hasColorVarying);
        scan(vf->params, phoskia::ir::IRShaderParam::Direction::Out,
             phoskia::PhoskiaSemantic::Position, plan.hasPositionVarying);
    }
    if (ff != nullptr) {
        scan(ff->inputs, phoskia::ir::IRShaderParam::Direction::In,
             phoskia::PhoskiaSemantic::Normal, plan.hasNormalVarying);
        scan(ff->inputs, phoskia::ir::IRShaderParam::Direction::In,
             phoskia::PhoskiaSemantic::Texcoord, plan.hasTexcoordVarying);
        scan(ff->inputs, phoskia::ir::IRShaderParam::Direction::In,
             phoskia::PhoskiaSemantic::Color, plan.hasColorVarying);
        scan(ff->inputs, phoskia::ir::IRShaderParam::Direction::In,
             phoskia::PhoskiaSemantic::Position, plan.hasPositionVarying);
    }
    return plan;
}

class VaryingSemanticAssigner {
public:
    explicit VaryingSemanticAssigner(const VaryingLayoutPlan& plan)
    {
        if (plan.hasColorVarying) {
            _map[phoskia::PhoskiaSemantic::Color] = "COLOR0";
        }
        if (plan.hasTexcoordVarying) {
            _map[phoskia::PhoskiaSemantic::Texcoord] = "TEXCOORD0";
            if (_nextTexCoord == 0) {
                _nextTexCoord = 1;
            }
        }
        if (plan.hasNormalVarying) {
            _map[phoskia::PhoskiaSemantic::Normal] = "NORMAL";
        }
        if (plan.hasPositionVarying) {
            assignNextTexCoord(phoskia::PhoskiaSemantic::Position);
        }
    }

    const char* lookup(phoskia::PhoskiaSemantic sem) const
    {
        const auto it = _map.find(sem);
        return it != _map.end() ? it->second.c_str() : "TEXCOORD0";
    }

private:
    std::unordered_map<phoskia::PhoskiaSemantic, std::string> _map;
    uint8_t _nextTexCoord = 0;

    void assignNextTexCoord(phoskia::PhoskiaSemantic sem)
    {
        _map[sem] = std::string("TEXCOORD") + std::to_string(_nextTexCoord++);
    }
};

const std::unordered_map<phoskia::PhoskiaSemantic, PhoskiaSemanticInfo>&
semanticTable() {
    static const std::unordered_map<phoskia::PhoskiaSemantic, PhoskiaSemanticInfo> table = {
        {phoskia::PhoskiaSemantic::Position, {"POSITION",  "a_position",  "v_position",  "vec3", "vec3(0.0, 0.0, 0.0)"}},
        {phoskia::PhoskiaSemantic::Normal,   {"NORMAL",    "a_normal",    "v_normal",    "vec3", "vec3(0.0, 0.0, 1.0)"}},
        {phoskia::PhoskiaSemantic::Color,    {"COLOR0",    "a_color0",    "v_color0",    "vec4", "vec4(1.0, 0.0, 0.0, 1.0)"}},
        {phoskia::PhoskiaSemantic::Texcoord, {"TEXCOORD0", "a_texcoord0", "v_texcoord0", "vec2", "vec2(0.0, 0.0)"}},
        // Phase 1 RD-03: skeletal skinning vertex attributes.
        // These map to bgfx's BLENDINDICES (4x u8 normalized) and
        // BLENDWEIGHT (4x f32) slots. Phoskia types here are vec4
        // for both — Phoskia does not have a distinct integer-vector
        // type for indices; the AYRenderer repack path takes care of
        // the per-component byte packing for the Indices channel.
        {phoskia::PhoskiaSemantic::BoneIndices, {"BLENDINDICES", "a_indices", "", "vec4", "vec4(0.0, 0.0, 0.0, 0.0)"}},
        {phoskia::PhoskiaSemantic::BoneWeights, {"BLENDWEIGHT",  "a_weights", "", "vec4", "vec4(0.0, 0.0, 0.0, 0.0)"}},
    };
    return table;
}

// RenameContext maps a Phoskia identifier to its bgfx-side counterpart.
// Built per-shader-block from the in/out ShaderParam declarations: the
// Phoskia name (`pos`, `uv`, ...) becomes the bgfx attr/varying name
// (`a_position`, `v_texcoord0`, ...). The converter passes the context
// into emitExpr so identifiers are rewritten on the fly — Phoskia source
// stays bgfx-agnostic.
struct RenameContext {
    std::unordered_map<std::string, std::string> map;
    std::unordered_map<std::string, phoskia::ir::SamplerKind> textureKinds;
    bool contains(const std::string& k) const { return map.find(k) != map.end(); }
    const std::string& lookup(const std::string& k) const {
        auto it = map.find(k);
        return it == map.end() ? k : it->second;
    }
};

// Map a GLSL / Phoskia type lexeme ("float", "vec3", "mat4", "int",
// "bool", ...) to the corresponding Phoskia Type shared_ptr. Returns
// nullptr for unrecognized lexemes (the caller registers nothing in
// that case — TypeInference will then treat the identifier as an
// unresolved TypeVar).
std::shared_ptr<phoskia::Type> phoskiaGLSLTypeToPhoskiaType(const std::string& lex) {
    using namespace phoskia;
    if (lex == "float")  return BuiltinTypes::Float;
    if (lex == "int")    return BuiltinTypes::Int;
    if (lex == "uint")   return BuiltinTypes::Uint;  // Phase 3.3 Block 1
    if (lex == "bool")   return BuiltinTypes::Bool;
    if (lex == "vec2")   return BuiltinTypes::Vec2();
    if (lex == "vec3")   return BuiltinTypes::Vec3();
    if (lex == "vec4")   return BuiltinTypes::Vec4();
    if (lex == "ivec2")  return std::make_shared<VectorType>(PrimitiveType::Int, 2);
    if (lex == "ivec3")  return std::make_shared<VectorType>(PrimitiveType::Int, 3);
    if (lex == "ivec4")  return std::make_shared<VectorType>(PrimitiveType::Int, 4);
    if (lex == "mat2")   return BuiltinTypes::Mat2();
    if (lex == "mat3")   return BuiltinTypes::Mat3();
    if (lex == "mat4")   return BuiltinTypes::Mat4();
    return nullptr;
}

bool exprIsMatrix(const std::shared_ptr<phoskia::Type>& type)
{
    return type && std::dynamic_pointer_cast<phoskia::MatrixType>(type) != nullptr;
}

bool binaryMulNeedsBgfxMul(const phoskia::ir::IRBinaryExpr& bin)
{
    if (bin.op.type != phoskia::TokenType::Star || !bin.left || !bin.right) {
        return false;
    }

    const auto looksLikeMatrix = [](const phoskia::ir::IRExpr* expr) -> bool {
        if (expr == nullptr) {
            return false;
        }
        if (exprIsMatrix(expr->resolvedType)) {
            return true;
        }
        if (auto id = dynamic_cast<const phoskia::ir::IRIdentifierExpr*>(expr)) {
            return phoskia::detail::frameBuiltinIsMatrix(id->name);
        }
        return false;
    };

    return looksLikeMatrix(bin.left.get()) || looksLikeMatrix(bin.right.get());
}

// Phase 3.1: emit* helpers consume IR nodes. Each IR expression already
// carries a pre-resolved type (filled by IRGenerator); backends read the
// type from `expr.resolvedType` directly instead of re-running inference.
void emitExpr(std::ostringstream& out, const phoskia::ir::IRExpr& e,
              const RenameContext& ctx);

void emitStmt(std::ostringstream& out, const phoskia::ir::IRStmt& s,
              const RenameContext& ctx, const char* outputVar,
              phoskia::TypeEnvironment& env);

void emitStmt(std::ostringstream& out, const phoskia::ir::IRStmt& s,
              const RenameContext& ctx, phoskia::TypeEnvironment& env) {
    emitStmt(out, s, ctx, nullptr, env);
}

void emitStmt(std::ostringstream& out, const phoskia::ir::IRStmt& s,
              const RenameContext& ctx) {
    // Forward-compat overload retained only for the property / texture
    // emitExpr paths which don't participate in the let-stmt type
    // inference. Always uses a throwaway env.
    phoskia::TypeEnvironment throwaway;
    emitStmt(out, s, ctx, nullptr, throwaway);
}

void emitStmt(std::ostringstream& out, const phoskia::ir::IRStmt& s,
              const RenameContext& ctx, const char* outputVar,
              phoskia::TypeEnvironment& env) {
    if (auto let = dynamic_cast<const phoskia::ir::IRLetStmt*>(&s)) {
        // GLSL (and bgfx's GLSL profile) requires an explicit type on
        // every local declaration. With Phase 3.1 the type is read
        // directly from `let->initializer->resolvedType` (populated by
        // IRGenerator). We then register the binding in `env` so later
        // statements that reference `let->name` see a concrete type
        // instead of a fresh TypeVar (chain-of-arithmetic propagation).
        std::string glslType;
        std::shared_ptr<phoskia::Type> resolvedType;
        if (let->initializer) {
            resolvedType = let->initializer->resolvedType;
            if (auto vec = std::dynamic_pointer_cast<phoskia::VectorType>(resolvedType)) {
                glslType = vec->toString();  // "vec2" / "vec3" / "vec4" / "ivec3" / ...
            } else if (auto mat = std::dynamic_pointer_cast<phoskia::MatrixType>(resolvedType)) {
                glslType = mat->toString();  // "mat2" / "mat3" / "mat4"
            } else if (auto p = std::dynamic_pointer_cast<phoskia::PrimitiveType_>(resolvedType)) {
                glslType = p->toString();   // "float" / "int" / "bool"
            }
        }
        // Register the binding for subsequent statements.
        if (resolvedType) {
            env.addVariable(let->name, resolvedType);
        }
        if (!glslType.empty()) {
            out << "    " << glslType << " " << let->name << " = ";
        } else {
            out << "    " << let->name << " = ";
        }
        if (let->initializer) emitExpr(out, *let->initializer, ctx);
        out << ";\n";
    } else if (auto ret = dynamic_cast<const phoskia::ir::IRReturnStmt*>(&s)) {
        out << "    ";
        // Phoskia source: `return <vec4-expr>`. The compiler implicitly
        // binds the return value to the block's output slot — `gl_Position`
        // for vertex, `gl_FragColor` for fragment. User source never
        // references these identifiers directly.
        if (outputVar) out << outputVar << " = ";
        if (ret->value) emitExpr(out, *ret->value, ctx);
        out << ";\n";
    } else if (auto es = dynamic_cast<const phoskia::ir::IRExprStmt*>(&s)) {
        if (es->expr) {
            out << "    ";
            emitExpr(out, *es->expr, ctx);
            out << ";\n";
        }
    }
    // ShaderParam / IfStmt / ForStmt / VariantAttribute at the body level
    // should not appear in IRVertexFunc::body / IRFragmentFunc::body —
    // ShaderParams live in params/inputs, VariantAttribute is consumed by
    // the per-block body loops in convertMaterial. IfStmt / ForStmt are
    // accepted by the IR but the BGFX backend has no path for them (the
    // previous AST path silently skipped them too).
}

void emitExpr(std::ostringstream& out, const phoskia::ir::IRExpr& e,
              const RenameContext& ctx) {
    if (auto bin = dynamic_cast<const phoskia::ir::IRBinaryExpr*>(&e)) {
        // Parenthesize the whole subexpression so operator precedence
        // is preserved in the emitted GLSL. Without the parens, a
        // chain like `a * (b - c) * d` (parsed as `((a * (b-c)) * d)`)
        // would emit as `a * b - c * d` and silently change the math
        // under GLSL's left-associative precedence rules. Wrapping
        // every BinaryExpr in (...) is verbose but always correct.
        //
        // Matrix * vector (or matrix * matrix) uses bgfx mul() so the
        // same .sc compiles under GLSL and HLSL/DXBC profiles.
        if (binaryMulNeedsBgfxMul(*bin)) {
            out << "mul(";
            emitExpr(out, *bin->left, ctx);
            out << ", ";
            emitExpr(out, *bin->right, ctx);
            out << ")";
        } else {
            out << "(";
            emitExpr(out, *bin->left, ctx);
            out << " " << bin->op.lexeme << " ";
            emitExpr(out, *bin->right, ctx);
            out << ")";
        }
    } else if (auto un = dynamic_cast<const phoskia::ir::IRUnaryExpr*>(&e)) {
        out << un->op.lexeme;
        emitExpr(out, *un->operand, ctx);
    } else if (auto call = dynamic_cast<const phoskia::ir::IRCallExpr*>(&e)) {
        // Builtin mapping: Phoskia's `sample(tex, uv)` → bgfx `texture2D(tex, uv)`.
        // PBR math functions (fresnelSchlick / distributionGGX /
        // geometrySchlickGGX / geometrySmith) registered as Phase 2
        // Step 3 builtins are inlined to plain GLSL expressions here
        // because bgfx's shaderc does not recognise the Phoskia
        // names. The TypeInference engine knows about these
        // signatures for type-checking; the converter handles
        // emission. After inline, the result is the same GLSL a
        // user would have written by hand from the math.
        if (auto callee = dynamic_cast<const phoskia::ir::IRIdentifierExpr*>(call->callee.get())) {
            if (callee->name == "sample") {
                phoskia::ir::SamplerKind samplerKind = phoskia::ir::SamplerKind::Sampler2D;
                if (!call->args.empty()) {
                    if (auto texId = dynamic_cast<const phoskia::ir::IRIdentifierExpr*>(
                            call->args[0].get())) {
                        const auto it = ctx.textureKinds.find(texId->name);
                        if (it != ctx.textureKinds.end()) {
                            samplerKind = it->second;
                        }
                    }
                }
                out << ((samplerKind == phoskia::ir::SamplerKind::SamplerCube)
                            ? "textureCube" : "texture2D");
            } else if (callee->name == "thread_id") {
                // Phase 3.2 Block 2: compute builtin.
                // Phoskia `thread_id()` → GLSL `gl_GlobalInvocationID`
                // (uvec3; the type-inference side registers thread_id
                // as vec3 so .x/.y/.z swizzles resolve to float).
                out << "gl_GlobalInvocationID";
                return;  // skip the generic `name(args)` emit below
            } else if (callee->name == "group_id") {
                // Phoskia `group_id()` → GLSL `gl_WorkGroupID`.
                out << "gl_WorkGroupID";
                return;
            } else if (callee->name == "dispatch_id") {
                // Phoskia `dispatch_id()` → GLSL `gl_NumWorkGroups * gl_WorkGroupID`.
                // The product gives a global workgroup-space index (one
                // per workgroup, not per thread); for per-thread
                // dispatch-space users want thread_id instead. We
                // surface both shapes via separate names rather than
                // asking callers to multiply by hand.
                out << "(gl_NumWorkGroups * gl_WorkGroupID)";
                return;
            } else if (callee->name == "fresnelSchlick") {
                // F(cosTheta, F0) = F0 + (1 - F0) * (1 - cosTheta)^5
                // Arity is enforced upstream by the SemanticAnalyzer and
                // the builtin's FunctionType; reaching this branch with
                // the wrong arg count means someone hand-built the AST,
                // in which case we still emit something compile-able
                // (a zero fallback) rather than crash the converter.
                if (call->args.size() != 2) {
                    out << "vec3(0.0)";
                } else {
                    // F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0)
                    // Note: vec3(1.0) is a constructor call (its own
                    // parens), so we open a paren before, let the
                    // constructor add the inner one, then close both.
                    out << "(";
                    emitExpr(out, *call->args[1], ctx);  // F0
                    out << " + (vec3(1.0) - ";
                    emitExpr(out, *call->args[1], ctx);
                    out << ") * pow(1.0 - ";
                    emitExpr(out, *call->args[0], ctx);  // cosTheta
                    out << ", 5.0))";
                }
                return;
            } else if (callee->name == "fresnelSchlickRoughness") {
                // F(cosTheta, F0, roughness) =
                //     F0 + (max(roughness^2, 1 - F0) - F0) * (1 - cosTheta)^5
                // (glTF KHR_materials_clearcoat form). Both `roughness^2`
                // and `1 - F0` are vec3; GLSL's `max` requires both
                // operands to share a type, so we promote the scalar
                // roughness^2 to vec3(roughness^2) before max.
                if (call->args.size() != 3) {
                    out << "vec3(0.0)";
                } else {
                    out << "(";
                    emitExpr(out, *call->args[1], ctx);  // F0
                    out << " + (max(vec3(";
                    emitExpr(out, *call->args[2], ctx);  // roughness
                    out << " * ";
                    emitExpr(out, *call->args[2], ctx);
                    out << "), vec3(1.0) - ";
                    emitExpr(out, *call->args[1], ctx);
                    out << ") - ";
                    emitExpr(out, *call->args[1], ctx);  // F0
                    out << ") * pow(1.0 - ";
                    emitExpr(out, *call->args[0], ctx);  // cosTheta
                    out << ", 5.0))";
                }
                return;
            } else if (callee->name == "distributionGGX") {
                // D(NdotH, roughness) = alpha^2 / (PI * (NdotH^2 * (alpha^2 - 1) + 1)^2)
                // The exponent is the scalar 2 (squaring the inner
                // expression); GLSL's pow(float, float) and
                // pow(vec, float) both work, so a scalar `2.0` is
                // shape-correct for both scalar and vector inputs.
                if (call->args.size() != 2) {
                    out << "0.0";
                } else {
                    // (roughness*roughness) / (3.14159265 * pow(NdotH*NdotH * (roughness*roughness - 1.0) + 1.0, 2.0))
                    // Paren structure (one `( ... )` per manual scope):
                    //   ( ... roughness*roughness / (3.14159265 * pow(... , 2.0)) )
                    //       └ 1 outer wrap ─┘   └ denom (   )  └ pow (   )     └ (roughness^2 - 1) (  )
                    out << "(";
                    emitExpr(out, *call->args[1], ctx);  // roughness
                    out << " * ";
                    emitExpr(out, *call->args[1], ctx);
                    out << " / (3.14159265 * pow(";
                    emitExpr(out, *call->args[0], ctx);  // NdotH
                    out << " * ";
                    emitExpr(out, *call->args[0], ctx);
                    out << " * (";
                    emitExpr(out, *call->args[1], ctx);
                    out << " * ";
                    emitExpr(out, *call->args[1], ctx);
                    out << " - 1.0) + 1.0, 2.0)))";
                }
                return;
            } else if (callee->name == "geometrySchlickGGX") {
                // G_sub(NdotV, roughness) = NdotV / (NdotV * (1 - k) + k)
                //   where k = (roughness + 1)^2 / 8
                if (call->args.size() != 2) {
                    out << "0.0";
                } else {
                    // NdotV / (NdotV * (1.0 - (roughness+1)^2 / 8) + (roughness+1)^2 / 8)
                    auto emitK = [&]() {
                        out << "((";
                        emitExpr(out, *call->args[1], ctx);  // roughness
                        out << " + 1.0) * (";
                        emitExpr(out, *call->args[1], ctx);
                        out << " + 1.0)) / 8.0";
                    };
                    out << "(";
                    emitExpr(out, *call->args[0], ctx);  // NdotV
                    out << " / (";
                    emitExpr(out, *call->args[0], ctx);
                    out << " * (1.0 - ";
                    emitK();
                    out << ") + ";
                    emitK();
                    out << "))";
                }
                return;
            } else if (callee->name == "geometrySmith") {
                // G(NdotV, NdotL, roughness) = G_sub(NdotV) * G_sub(NdotL)
                //   (the G_sub formula is the same as geometrySchlickGGX
                //   applied to each side, with k derived from roughness)
                if (call->args.size() != 3) {
                    out << "0.0";
                } else {
                    // (NdotV / (NdotV*(1.0 - k) + k)) * (NdotL / (NdotL*(1.0 - k) + k))
                    //   where k = (roughness+1)^2 / 8
                    auto emitK = [&]() {
                        out << "((";
                        emitExpr(out, *call->args[2], ctx);
                        out << " + 1.0) * (";
                        emitExpr(out, *call->args[2], ctx);
                        out << " + 1.0)) / 8.0";
                    };
                    auto emitGSub = [&](const phoskia::ir::IRExpr& ndot) {
                        out << "(";
                        emitExpr(out, ndot, ctx);
                        out << " / (";
                        emitExpr(out, ndot, ctx);
                        out << " * (1.0 - ";
                        emitK();
                        out << ") + ";
                        emitK();
                        out << "))";
                    };
                    out << "(";
                    emitGSub(*call->args[0]);  // NdotV
                    out << " * ";
                    emitGSub(*call->args[1]);  // NdotL
                    out << ")";
                }
                return;
            } else if (callee->name == "skinningMatrix") {
                // Phase 1 RD-03: linear-blend skinning.
                //   skinningMatrix(indices, weights, bones, pos) →
                //     weights.x * bones[int(indices.x)] * pos
                //   + weights.y * bones[int(indices.y)] * pos
                //   + weights.z * bones[int(indices.z)] * pos
                //   + weights.w * bones[int(indices.w)] * pos
                // `bones` is the third argument's emitted expression —
                // for `Skeleton.bones` (mat4[] UBO field) the emit
                // produces `Skeleton.bones`, valid as an array accessor
                // in GLSL. Indices come in as ivec4; cast to int for
                // older GLSL profiles.
                if (call->args.size() != 4) {
                    out << "/* skinningMatrix: expected 4 args */ vec4(0.0)";
                    return;
                }
                const char* weightChannels = "xyzw";
                out << "(";
                for (int c = 0; c < 4; ++c) {
                    if (c > 0) out << " + ";
                    out << "(";
                    emitExpr(out, *call->args[1], ctx);  // weights
                    out << "." << weightChannels[c] << ") * (";
                    emitExpr(out, *call->args[2], ctx);  // bones
                    out << "[int(";
                    emitExpr(out, *call->args[0], ctx);  // indices
                    out << "." << weightChannels[c] << ")] * ";
                    emitExpr(out, *call->args[3], ctx);  // pos
                    out << ")";
                }
                out << ")";
                return;
            } else {
                out << callee->name;
            }
        } else {
            emitExpr(out, *call->callee, ctx);
        }
        out << "(";
        for (size_t i = 0; i < call->args.size(); ++i) {
            if (i > 0) out << ", ";
            emitExpr(out, *call->args[i], ctx);
        }
        out << ")";
    } else if (auto ident = dynamic_cast<const phoskia::ir::IRIdentifierExpr*>(&e)) {
        // Apply rename map for in-param identifiers (Phoskia → bgfx).
        // Compute thread-id builtins inline so Phoskia source can use
        // either `thread_id.x` (bare identifier) or `thread_id().x`
        // (call form) — both lower to the same GLSL builtin. The call
        // form is handled in the IRCallExpr branch above; this branch
        // covers the bare-identifier form (e.g. when a member access
        // skips the call wrapper). Rename-context lookup first so user
        // variables named `thread_id` would shadow the builtin, but
        // that's the user's choice (no Phoskia keyword reserves the
        // name).
        const std::string& lookupName = ctx.lookup(ident->name);
        if (lookupName == "thread_id") {
            out << "gl_GlobalInvocationID";
        } else if (lookupName == "group_id") {
            out << "gl_WorkGroupID";
        } else if (lookupName == "dispatch_id") {
            out << "(gl_NumWorkGroups * gl_WorkGroupID)";
        } else if (phoskia::detail::isFrameBuiltin(lookupName)) {
            out << phoskia::detail::bgfxFrameBuiltinExpr(lookupName);
        } else {
            out << lookupName;
        }
    } else if (auto lit = dynamic_cast<const phoskia::ir::IRLiteralExpr*>(&e)) {
        std::visit([&out](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                out << "0";
            } else if constexpr (std::is_same_v<T, bool>) {
                out << (arg ? "true" : "false");
            } else if constexpr (std::is_same_v<T, float>) {
                // Emit with a trailing ".0" if integral so GLSL parses as float.
                // The integral case must rebuild from the int form — std::ostream
                // would otherwise drop the decimal (e.g. `1.0f` → "1").
                if (arg == static_cast<float>(static_cast<int>(arg))) {
                    out << static_cast<int>(arg) << ".0";
                } else {
                    out << arg;
                }
            } else if constexpr (std::is_same_v<T, int>) {
                out << arg;
            } else if constexpr (std::is_same_v<T, std::string>) {
                out << "\"" << arg << "\"";
            }
        }, lit->value);
    } else if (auto mem = dynamic_cast<const phoskia::ir::IRMemberExpr*>(&e)) {
        emitExpr(out, *mem->object, ctx);
        out << "." << mem->member;
    } else if (auto idx = dynamic_cast<const phoskia::ir::IRIndexExpr*>(&e)) {
        emitExpr(out, *idx->object, ctx);
        out << "[";
        emitExpr(out, *idx->index, ctx);
        out << "]";
    }
}

const phoskia::ir::IRShaderParam* findInParam(const std::vector<phoskia::ir::IRStmtPtr>& params,
                                              phoskia::PhoskiaSemantic sem) {
    for (const auto& s : params) {
        auto* p = dynamic_cast<const phoskia::ir::IRShaderParam*>(s.get());
        if (p && p->dir == phoskia::ir::IRShaderParam::Direction::In && p->semantic == sem) {
            return p;
        }
    }
    return nullptr;
}

const phoskia::ir::IRShaderParam* findOutParam(const std::vector<phoskia::ir::IRStmtPtr>& params,
                                               phoskia::PhoskiaSemantic sem) {
    for (const auto& s : params) {
        auto* p = dynamic_cast<const phoskia::ir::IRShaderParam*>(s.get());
        if (p && p->dir == phoskia::ir::IRShaderParam::Direction::Out && p->semantic == sem) {
            return p;
        }
    }
    return nullptr;
}

// Translate a Phoskia variant name like `useEmission` into a C-preprocessor
// macro identifier acceptable to GLSL/HLSL/SPIR-V: BGFX_VARIANT_USE_EMISSION.
// All non-alphanumeric characters become underscores; lowercase letters
// are uppercased. camelCase boundaries (lower→upper, letter→digit) insert
// an underscore so `useEmission` → `USE_EMISSION`, `HDR2Pass` → `HDR2_PASS`,
// `alpha-test` → `ALPHA_TEST`.
static std::string variantMacroName(const std::string& name) {
    std::string out;
    out.reserve(name.size() + 16);
    out += "BGFX_VARIANT_";
    auto isUpper = [](char c) { return c >= 'A' && c <= 'Z'; };
    auto isLower = [](char c) { return c >= 'a' && c <= 'z'; };
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    auto isAlnum = [&](char c) { return isUpper(c) || isLower(c) || isDigit(c); };
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        // Insert underscore only at lowercase → uppercase boundaries
        // (camelCase word breaks like `useEmission` → `USE_EMISSION`).
        // Digit boundaries are NOT split: `HDR2Pass` stays `HDR2PASS`
        // (digits flow together with the surrounding letters). Non-alnum
        // characters always become `_` (`alpha-test` → `ALPHA_TEST`).
        if (i > 0 && isLower(name[i - 1]) && isUpper(c)) {
            out += '_';
        }
        if (isLower(c)) {
            out += static_cast<char>(c - 'a' + 'A');
        } else if (isUpper(c) || isDigit(c)) {
            out += c;
        } else {
            out += '_';
        }
    }
    return out;
}

// Infer the GLSL type string of a property initializer. Returns "vec4"
// when inference can't recover a concrete type (the historical
// Phase 1 default — chosen because every PBR demo so far used vec4
// properties; tests that need a non-vec4 property now drive this
// from the initializer shape).
//
// Phase 3.1: the IRGenerator pre-attaches `resolvedType` to the
// property initializer, so we read the type directly instead of
// running a fresh TypeInference pass.
std::string inferPropertyGLSLType(const phoskia::ir::IRDeclaration& decl) {
    std::string glslType = "vec4";
    if (decl.kind != phoskia::ir::IRDeclaration::Kind::Property) return glslType;
    if (!decl.propertyInit) return glslType;
    auto concrete = decl.propertyInit->resolvedType;
    if (auto vec = std::dynamic_pointer_cast<phoskia::VectorType>(concrete)) {
        glslType = vec->toString();
    } else if (auto mat = std::dynamic_pointer_cast<phoskia::MatrixType>(concrete)) {
        glslType = mat->toString();
    } else if (auto p = std::dynamic_pointer_cast<phoskia::PrimitiveType_>(concrete)) {
        glslType = p->toString();
    }
    return glslType;
}

void emitPropertyUniform(std::ostream& out, const phoskia::ir::IRDeclaration& decl) {
    std::string glslType = inferPropertyGLSLType(decl);
    out << "uniform " << glslType << " " << decl.name << " = ";
    if (decl.propertyInit) {
        std::ostringstream tmp;
        RenameContext empty;  // properties don't reference in/out params
        emitExpr(tmp, *decl.propertyInit, empty);
        out << tmp.str();
    } else {
        out << "0.0";
    }
    out << ";\n";
}

} // namespace

// --------------------------------------------------------------------------
// Top-level conversion
// --------------------------------------------------------------------------

BGFXConvertResult AYBGFXConverter::convertBGFX(const phoskia::ir::IRProgram& program) {
    // Phase 3.2-pre SSO/NRVO fix: same pattern as Compiler::compile.
    // Return-by-value of BGFXConvertResult corrupts caller-stack on
    // MSVC under certain optimizer decisions. Forward to the out-param
    // overload.
    BGFXConvertResult out;
    convertBGFX(program, out);
    return out;
}

void AYBGFXConverter::convertBGFX(const phoskia::ir::IRProgram& program, BGFXConvertResult& out) {
    out = BGFXConvertResult{};
    out.success = true;

    // Phase 3.4 + 3.5-B: pre-emit UBO decls once. The same string is
    // spliced into every material's vs/fs and every compute's cs. GLSL
    // allows the same `uniform Name { ... } Name;` block in multiple
    // shader stages; the GLSL compiler dedupes when needed.
    _uboDecls.clear();
    _uniformBlocks.clear();
    // Phase 3.5-A: storage buffer binding info is collected per-compute
    // (each compute may have its own set of storage decls with their
    // own binding slots). We clear at convertBGFX entry so a fresh
    // top-level invocation doesn't see leftovers from a previous call.
    _storageBuffers.clear();

    // Phase 3.5-B: detect duplicate UBO bindings BEFORE emit. UBO
    // bindings are global per-program (UBOs are shared across
    // vs/fs/cs and live in IRProgram::uniformBlocks flat vector).
    // Mirrors the storage duplicate-detection pattern in
    // convertComputeDecl (lines 1077-1090) but the scope here is the
    // whole program, not a single compute.
    {
        std::unordered_map<int, std::string> usedBindings;
        bool dup = false;
        for (const auto& ub : program.uniformBlocks) {
            if (!ub || ub->uboBinding < 0) continue;  // -1 = auto, skip
            auto it = usedBindings.find(ub->uboBinding);
            if (it != usedBindings.end()) {
                out.errors.push_back("UniformBlock '" + ub->name +
                    "' has duplicate binding " +
                    std::to_string(ub->uboBinding) +
                    " (also used by '" + it->second + "')");
                dup = true;
            } else {
                usedBindings[ub->uboBinding] = ub->name;
            }
        }
        if (dup) {
            out.success = false;
            return;  // bail early — don't emit partial layout lines
        }
    }

    // Phase 3.5-B: auto-assign binding slots to UBOs without explicit
    // binding. Start from max(explicit) + 1 (or 0 if none explicit).
    // Mirrors the storage auto-slot heuristic in convertComputeDecl.
    int nextAutoBinding = 0;
    for (const auto& ub : program.uniformBlocks) {
        if (!ub) continue;
        if (ub->uboBinding >= 0 && ub->uboBinding >= nextAutoBinding) {
            nextAutoBinding = ub->uboBinding + 1;
        }
    }

    for (const auto& ub : program.uniformBlocks) {
        if (!ub) continue;
        std::ostringstream blockSrc;
        int binding = ub->uboBinding;            // -1 = auto
        if (binding < 0) binding = nextAutoBinding++;
        blockSrc << "layout(std140, binding = " << binding
                 << ") uniform " << ub->name << " {\n";
        for (size_t i = 0; i < ub->uboFields.size(); ++i) {
            std::string fieldTypeLex = "vec4";  // fallback (matches IR warning policy)
            if (ub->uboFields[i]) fieldTypeLex = ub->uboFields[i]->toString();
            // Phase 1 RD-04: emit `mat4 bones[N];` when arrayLength>0.
            // Strip the "array<...>" wrapping the IR Type inserts — GLSL
            // wants the raw element type plus `[N]`. Also normalize
            // `mat4x4` → `mat4` (single-number form is what bgfx's GLSL
            // 1.20 profile accepts).
            const int arrayLength = (i < ub->uboFieldArrayLengths.size())
                ? ub->uboFieldArrayLengths[i] : 0;
            std::string elementLex = fieldTypeLex;
            const auto openBracket = elementLex.find("array<");
            if (openBracket == 0) {
                const auto closeAngle = elementLex.find('>');
                if (closeAngle != std::string::npos) {
                    elementLex = elementLex.substr(openBracket + 6,
                        closeAngle - openBracket - 6);
                    const auto comma = elementLex.find(',');
                    if (comma != std::string::npos) {
                        elementLex = elementLex.substr(0, comma);
                    }
                }
            }
            if (elementLex == "mat4x4") elementLex = "mat4";
            else if (elementLex == "mat3x3") elementLex = "mat3";
            else if (elementLex == "mat2x2") elementLex = "mat2";
            blockSrc << "    " << elementLex << " " << ub->uboFieldNames[i];
            if (arrayLength > 0) {
                blockSrc << "[" << arrayLength << "]";
            }
            blockSrc << ";\n";
        }
        blockSrc << "} " << ub->name << ";\n\n";
        _uboDecls += blockSrc.str();

        BGFXUniformBlock bub;
        bub.name = ub->name;
        bub.binding = binding;
        bub.fieldNames = ub->uboFieldNames;

        // Phase 1 RD-04: feed arrayLengths through to std140 layout so
        // total block size accounts for N*elementSize.
        std::vector<std::string> fieldTypes;
        fieldTypes.reserve(ub->uboFields.size());
        std::vector<size_t> fieldArrayLengths;
        fieldArrayLengths.reserve(ub->uboFields.size());
        for (size_t i = 0; i < ub->uboFields.size(); ++i) {
            std::string fieldTypeLex = "vec4";
            if (ub->uboFields[i]) {
                fieldTypeLex = ub->uboFields[i]->toString();
            }
            // Strip "array<T, N>" / "array<T>" wrapping for std140 — the
            // helper wants the bare element type plus the parallel
            // arrayLengths vector (we keep that already). Also normalize
            // `mat4x4` → `mat4` etc. so the std140 table recognizes them.
            const auto openBracket = fieldTypeLex.find("array<");
            if (openBracket == 0) {
                const auto closeAngle = fieldTypeLex.find('>');
                if (closeAngle != std::string::npos) {
                    fieldTypeLex = fieldTypeLex.substr(openBracket + 6,
                        closeAngle - openBracket - 6);
                    const auto comma = fieldTypeLex.find(',');
                    if (comma != std::string::npos) {
                        fieldTypeLex = fieldTypeLex.substr(0, comma);
                    }
                }
            }
            if (fieldTypeLex == "mat4x4") fieldTypeLex = "mat4";
            else if (fieldTypeLex == "mat3x3") fieldTypeLex = "mat3";
            else if (fieldTypeLex == "mat2x2") fieldTypeLex = "mat2";
            fieldTypes.push_back(fieldTypeLex);
            const int al = (i < ub->uboFieldArrayLengths.size())
                ? ub->uboFieldArrayLengths[i] : 0;
            fieldArrayLengths.push_back(static_cast<size_t>(al > 0 ? al : 1));
        }

        detail::Std140Layout layout;
        std::string layoutError;
        if (!detail::computeStd140Layout(bub.fieldNames, fieldTypes, layout,
                                          &layoutError, fieldArrayLengths)) {
            out.errors.push_back("UniformBlock '" + ub->name + "': " + layoutError);
            out.success = false;
            return;
        }

        bub.sizeBytes = layout.sizeBytes;
        bub.members.reserve(layout.members.size());
        for (const detail::Std140Member& member : layout.members) {
            BGFXUniformBlockMember outMember;
            outMember.name = member.name;
            outMember.type = member.type;
            outMember.offsetBytes = member.offsetBytes;
            outMember.sizeBytes = member.sizeBytes;
            bub.members.push_back(std::move(outMember));
        }

        _uniformBlocks.push_back(std::move(bub));
    }

    try {
        for (const auto& mat : program.materials) {
            if (mat) out.materialStages.push_back(convertMaterial(*mat));
        }
        for (const auto& cmp : program.computes) {
            if (!cmp) continue;
            // Phase 3.2: BGFX .sc IS the compute target backend.
            // bgfx 1.18 + shaderc 1.18 fully support compute via
            //   shaderc --type compute -o <out.bin> <input.sc>
            //   bgfx::createProgram(ShaderHandle _csh)
            //   bgfx::dispatch(ProgramHandle, numGroupsX, numGroupsY, numGroupsZ)
            // The earlier "BGFX .sc does not support compute" diagnostic
            // was Phase 2.5-era speculation, never empirically verified,
            // and is incorrect. Compute declarations are now first-class.
            out.computeStages.push_back(convertComputeDecl(*cmp));
        }
    } catch (const std::exception& e) {
        out.errors.push_back(e.what());
        out.success = false;
    } catch (...) {
        out.errors.push_back("Unknown exception during BGFX conversion");
        out.success = false;
    }

    out.uniforms = _uniforms;
    out.textures = _textures;
    out.uniformBlocks = _uniformBlocks;
    // Phase 3.5-A: flush storage buffer binding info collected during
    // convertComputeDecl (one BGFXStorageBuffer per storage decl,
    // with binding resolved including auto-assigned slots).
    out.storageBuffers = _storageBuffers;
}

// Phase 3.6 productization entry point. See AYBGFXConverter.h for the
// full contract. Implementation flow:
//   1. Run convertBGFX() to populate out.materialStages / .computeStages
//      and the binding metadata. Bail with success=false on any
//      convertBGFX error (duplicate bindings, unknown exceptions).
//   2. Lazy-init the cached AYShadercDriver (first call only). If
//      shaderc is missing, the driver ctor throws std::runtime_error;
//      we catch and surface it in out.errors so the frontend never
//      sees an exception.
//   3. For each material: compile vs (--type vertex) and fs
//      (--type fragment --varyingdef). Read the resulting .bin bytes
//      into out.vsBin / out.fsBin.
//   4. For each compute: compile cs (--type compute). Bytes go into
//      out.csBin.
//   5. If opts.keepSources: fill out.sources with the .sc text per
//      stage (same keying as the plan in design.md §8.4).
//   6. If opts.dumpIntermediate: write each .sc file to
//      opts.dumpDir/<key>.sc. Failures here are non-fatal — logged
//      to out.errors but success stays true if shaderc succeeded.
//
// We do NOT re-run emit() to recover the .sc text — out.materialStages
// already carries the exact strings convertBGFX emitted (the same
// strings the legacy BGFXShaderFiles.vs/fs/varyingDef fields used to
// expose). Reading them is cheaper than a second convertBGFX pass and
// guarantees bit-identical input to shaderc.
namespace {

// stat()-based existence check, now backed by AYIO. We previously hand-rolled
// stat() to dodge std::filesystem::exists name pollution; AYIO's
// Directory::exists is the canonical replacement.
bool dirExists(const std::string& p) {
    if (p.empty()) return false;
    return ayt::io::Directory::exists(p);
}

bool ensureDumpDirExists(const std::string& p) {
    if (p.empty()) {
        return false;
    }
    if (dirExists(p)) {
        return true;
    }
    return ayt::io::Directory::createRecursive(p);
}

// Best-effort write of one .sc debug file. Failures surface in
// program.warnings (not errors) — the .bin bytes were already produced
// successfully and dump failures shouldn't poison the success flag.
//
// AYIO migration: writeAllBytes + path::join replace std::ofstream +
// manual string concat. atomicWrite is overkill here (debug dumps are
// re-runnable and a crash mid-dump just means a truncated .sc which the
// user can delete), so we stick with the simpler writeAllBytes path.
void dumpScFile(const std::string& dumpDir,
                const std::string& key,
                const std::string& source,
                std::vector<std::string>& warnings) {
    const std::string path = ayt::io::path::join(dumpDir, key);
    if (!ayt::io::File::writeAllText(path, source)) {
        warnings.push_back("dumpIntermediate: cannot write " + path +
                           " (does dumpDir exist?)");
    }
}

} // namespace

void AYBGFXConverter::compileToBinary(const phoskia::ir::IRProgram& program,
                                      const BGFXCompileOptions& opts,
                                      CompiledShaderProgram& out) {
    out = CompiledShaderProgram{};

    // 1) Convert .sc sources via the existing path.
    BGFXConvertResult conv;
    convertBGFX(program, conv);
    out.uniformBlocks   = conv.uniformBlocks;
    out.storageBuffers  = conv.storageBuffers;
    out.uniforms        = conv.uniforms;
    out.textures        = conv.textures;
    if (!conv.success) {
        out.errors = conv.errors;
        out.success = false;
        return;
    }

    // 1a) Phase 3.6 Commit 5: when keepSources=true, populate the
    //     in-memory sources map immediately after convertBGFX
    //     succeeds — independent of shaderc availability. The .sc
    //     strings produced by convertBGFX are complete and stable
    //     (they're the same strings the post-Phase-3.6 frontend tests
    //     want to read instead of `ConvertResult::output`). Frontend
    //     tests must NOT depend on shaderc being installed to inspect
    //     the emit shape.
    if (opts.keepSources) {
        for (size_t i = 0; i < conv.materialStages.size(); ++i) {
            const auto& mf = conv.materialStages[i];
            out.sources[detail::vertexStageKey(i)] = mf.vertex;
            out.sources[detail::fragmentStageKey(i)] = mf.fragment;
        }
        if (!conv.materialStages.empty()) {
            out.sources[detail::kVaryingDefinitionsKey] =
                conv.materialStages.back().varyingDefinitions;
        }
        for (size_t i = 0; i < conv.computeStages.size(); ++i) {
            out.sources[detail::computeStageKey(i)] = conv.computeStages[i].compute;
        }
    }

    // 2) Lazy-init the shaderc driver. Per-call
    //    `opts.shadercPath` (if non-empty) wins over the
    //    process-wide default configured via
    //    `AYShadercDriver::setDefaultExecutable()`. Empty
    //    `opts.shadercPath` → default ctor (uses the global).
    //    Either path can throw on missing / unconfigured
    //    executable — catch and surface so the frontend never
    //    sees an exception.
    if (!_driver) {
        try {
            if (!opts.shadercPath.empty()) {
                _driver = std::make_unique<AYShadercDriver>(opts.shadercPath);
            } else {
                _driver = std::make_unique<AYShadercDriver>();  // uses global default
            }
        } catch (const std::exception& e) {
            out.errors.push_back(std::string("AYShadercDriver: ") + e.what());
            out.success = false;
            return;
        }
    }

    // 3) Compile per-stage. Failures short-circuit (shaderc errors
    //    are usually diagnostic enough — partial .bin bytes would
    //    be misleading to ship to the frontend).
    auto compileStage = [&](const std::string& stage,
                            const std::string& source,
                            const std::string& varyingdefSource,
                            const std::string& outputName,
                            std::vector<uint8_t>& dst) {
        ShaderCompileRequest req;
        req.scSource        = source;
        req.stage           = stage;
        req.varyingdefSource = varyingdefSource;
        req.platform        = opts.platform;
        req.profile         = opts.profile;
        req.includeDirs     = opts.includeDirs;
        req.defines         = opts.defines;
        req.outputName      = outputName;
        ShaderCompileResult r = _driver->compile(req);
        if (!r.ok) {
            out.errors.push_back("shaderc (" + outputName + "): " + r.stderrText);
            return false;
        }
        dst = std::move(r.bytes);
        return true;
    };

    bool allOk = true;

    // 3a) Materials → vs + fs.
    //
    // Note: the current CompiledShaderProgram shape holds one vsBin /
    // fsBin / csBin per program (not per material). This matches the
    // locked design in design.md §8.4 and the e2e test cardinality
    // (one material per test). Multi-material sources compile every
    // material's vs/fs and only the LAST material's bytes are kept in
    // the struct — earlier materials' bytes get overwritten. The
    // debug `sources` map still carries every material's .sc text, so
    // no information is lost on the debug path. Phase 4 will revisit
    // when `bgfx::createProgram` wire-up needs to feed multiple
    // materials' binaries to bgfx; the likely move is a
    // vector<vector<uint8_t>> shape. For Commit 2 / 3.6 the single
    // shape is locked.
    for (size_t i = 0; i < conv.materialStages.size(); ++i) {
        const auto& mf = conv.materialStages[i];
        const std::string vsKey = detail::vertexStageKey(i);
        const std::string fsKey = detail::fragmentStageKey(i);
        const std::string vdKey = detail::kVaryingDefinitionsKey;

        // vs (with varyingdef — required for HLSL/DXBC so attributes like a_position are declared)
        if (!compileStage("vertex", mf.vertex, mf.varyingDefinitions,
                          "material_" + std::to_string(i) + "_vs",
                          out.vsBin)) {
            allOk = false;
            break;
        }

        // fs (with varyingdef — bgfx shaderc wants it for fragment stage)
        if (!compileStage("fragment", mf.fragment, mf.varyingDefinitions,
                          "material_" + std::to_string(i) + "_fs",
                          out.fsBin)) {
            allOk = false;
            break;
        }

        // 5) In-memory debug sources: populated above (after convertBGFX
        //    succeeded) regardless of shaderc availability. See the
        //    pre-shaderc block at the top of compileToBinary — keeping
        //    that populate point before the shaderc call makes the
        //    debug map usable even on hosts where shaderc isn't
        //    installed (frontend tests want to read .sc strings to
        //    verify the emit shape without a real shaderc install).

        // 6) Disk dump (best-effort).
        if (opts.dumpIntermediate && !opts.dumpDir.empty()
            && ensureDumpDirExists(opts.dumpDir)) {
            dumpScFile(opts.dumpDir, vsKey, mf.vertex, out.warnings);
            dumpScFile(opts.dumpDir, fsKey, mf.fragment, out.warnings);
            if (i == 0) {
                dumpScFile(opts.dumpDir, vdKey, mf.varyingDefinitions, out.warnings);
            }
        }
    }

    // 3b) Compute → cs. Same single-csBin-per-program limitation.
    if (allOk) {
        for (size_t i = 0; i < conv.computeStages.size(); ++i) {
            const auto& cf = conv.computeStages[i];
            const std::string csKey = detail::computeStageKey(i);
            if (!compileStage("compute", cf.compute, "",
                              "compute_" + std::to_string(i),
                              out.csBin)) {
                allOk = false;
                break;
            }
            // In-memory sources already populated in the pre-shaderc
            // block above (right after convertBGFX succeeded) so
            // frontend tests can inspect .sc strings without a real
            // shaderc install. Dump-only branch follows.
            if (opts.dumpIntermediate && !opts.dumpDir.empty()
                && ensureDumpDirExists(opts.dumpDir)) {
                dumpScFile(opts.dumpDir, csKey, cf.compute, out.warnings);
            }
        }
    }

    // Success is determined purely by shaderc producing .bin bytes for
    // every required stage. DumpIntermediate failures live in
    // `out.warnings` (separate channel — see dumpScFile() above) so
    // they don't downgrade success.
    out.success = allOk;
}

ConvertResult AYBGFXConverter::convert(const phoskia::ir::IRProgram& program) {
    ConvertResult result;
    BGFXConvertResult bgfx;
    convertBGFX(program, bgfx);
    result.success = bgfx.success;
    result.errors = bgfx.errors;
    result.uniforms.reserve(bgfx.uniforms.size());
    for (const auto& u : bgfx.uniforms) {
        result.uniforms.emplace_back(u.name, u.type);
    }
    result.textures.reserve(bgfx.textures.size());
    for (const auto& t : bgfx.textures) {
        BackendTextureInfo bti;
        bti.name = t.name;
        bti.binding = t.binding;
        result.textures.push_back(bti);
    }
    return result;
}

// --------------------------------------------------------------------------
// Per-material conversion
// --------------------------------------------------------------------------

detail::BGFXMaterialStages AYBGFXConverter::convertMaterial(const phoskia::ir::IRMaterialDecl& mat) {
    _uniformDecls.clear();
    _textureDecls.clear();
    _propertyUniforms.clear();
    auto uniformSave = _uniforms;
    auto textureSave = _textures;
    _uniforms.clear();
    _textures.clear();

    // Per-block type environments — must be constructed BEFORE the first
    // pass so we can register uniforms / properties / textures into
    // them as we discover them. The let-stmt type inference in the
    // body relies on these bindings; without them, a uniform like
    // `uniform float roughness` referenced in `let r2 = roughness *
    // roughness` falls back to an unresolved TypeVar and the
    // converter emits GLSL without a type prefix.
    phoskia::TypeEnvironment vsEnv;
    phoskia::TypeEnvironment fsEnv;
    phoskia::detail::registerFrameBuiltins(vsEnv);
    phoskia::detail::registerFrameBuiltins(fsEnv);
    RenameContext vsCtx;
    RenameContext fsCtx;

    // Phase 3.1: IR separates uniform / property / texture into a
    // discriminated IRDeclaration, and vertex / fragment are first-class
    // fields on IRMaterialDecl — no more two-pass scan.
    for (const auto& decl : mat.declarations) {
        if (!decl) continue;
        switch (decl->kind) {
            case phoskia::ir::IRDeclaration::Kind::Uniform: {
                // The IR carries the uniformType as a Type pointer
                // (target-agnostic). For BGFX we need the GLSL lexeme
                // string to emit `uniform <type> <name>;`. The
                // Type->toString() methods on PrimitiveType_/VectorType
                // produce "float"/"vec3" as expected. MatrixType
                // produces "mat4x4" (canonical GLSL form) but bgfx's
                // GLSL 1.20 profile accepts the single-number form
                // "mat4" — fall back to that for square matrices so
                // the emitted uniform matches the user-written source.
                std::string glslLex = "vec4";
                if (decl->uniformType) {
                    if (auto mat = std::dynamic_pointer_cast<phoskia::MatrixType>(decl->uniformType)) {
                        if (mat->rows() == mat->cols()) {
                            glslLex = "mat" + std::to_string(mat->rows());
                        } else {
                            glslLex = mat->toString();  // "mat3x4" etc.
                        }
                    } else {
                        glslLex = decl->uniformType->toString();
                    }
                }
                _uniformDecls += "uniform " + glslLex + " " + decl->name + ";\n";
                BGFXUniform bu; bu.name = decl->name; bu.type = glslLex;
                _uniforms.push_back(std::move(bu));
                if (decl->uniformType) {
                    vsEnv.addVariable(decl->name, decl->uniformType);
                    fsEnv.addVariable(decl->name, decl->uniformType);
                }
                break;
            }
            case phoskia::ir::IRDeclaration::Kind::Texture: {
                uint8_t slot = static_cast<uint8_t>(_textures.size());
                std::string macro;
                std::string glslType;
                switch (decl->samplerKind) {
                case phoskia::ir::SamplerKind::SamplerCube:
                    macro = "SAMPLERCUBE(" + decl->name + ", " + std::to_string(slot) + ");\n";
                    glslType = "samplerCube";
                    break;
                case phoskia::ir::SamplerKind::Sampler2D:
                default:
                    macro = "SAMPLER2D(" + decl->name + ", " + std::to_string(slot) + ");\n";
                    glslType = "sampler2D";
                    break;
                }
                _textureDecls += macro;
                BGFXTexture bt;
                bt.name = decl->name;
                bt.binding = slot;
                bt.textureType = glslType;
                _textures.push_back(std::move(bt));
                vsCtx.textureKinds[decl->name] = decl->samplerKind;
                fsCtx.textureKinds[decl->name] = decl->samplerKind;
                // Textures are opaque to the type system (Phase 2 Step 2
                // deferred TextureType); register as Dynamic so lookup
                // succeeds even though sample() body-side checks pass
                // through the builtin registry directly.
                vsEnv.addVariable(decl->name, phoskia::BuiltinTypes::Dynamic);
                fsEnv.addVariable(decl->name, phoskia::BuiltinTypes::Dynamic);
                break;
            }
            case phoskia::ir::IRDeclaration::Kind::Property: {
                std::ostringstream tmp;
                emitPropertyUniform(tmp, *decl);
                _propertyUniforms += tmp.str();
                std::string glslType = inferPropertyGLSLType(*decl);
                BGFXUniform bu; bu.name = decl->name; bu.type = glslType;
                _uniforms.push_back(std::move(bu));
                // Register in both per-block type envs so the let-stmt
                // type inference in either block resolves references to
                // this property to the right GLSL type.
                std::shared_ptr<phoskia::Type> ptype;
                if      (glslType == "vec2") ptype = phoskia::BuiltinTypes::Vec2();
                else if (glslType == "vec3") ptype = phoskia::BuiltinTypes::Vec3();
                else if (glslType == "vec4") ptype = phoskia::BuiltinTypes::Vec4();
                if (ptype) {
                    vsEnv.addVariable(decl->name, ptype);
                    fsEnv.addVariable(decl->name, ptype);
                }
                break;
            }
        }
    }

    const phoskia::ir::IRVertexFunc*   vf = mat.vertex.get();
    const phoskia::ir::IRFragmentFunc* ff = mat.fragment.get();

    if (!vf) {
        _uniforms = std::move(uniformSave);
        _textures = std::move(textureSave);
        throw std::logic_error("Material '" + mat.name + "' is missing a 'vertex' block");
    }
    if (!ff) {
        _uniforms = std::move(uniformSave);
        _textures = std::move(textureSave);
        throw std::logic_error("Material '" + mat.name + "' is missing a 'fragment' block");
    }

    const auto& tbl = semanticTable();
    const VaryingLayoutPlan varyingPlan = computeVaryingLayout(vf, ff);
    const VaryingSemanticAssigner varyingSemantics(varyingPlan);

    // Populate the rename maps + already-constructed per-block
    // TypeEnvironments with in/out params (e.g. `let N = normalize(nrm)`
    // needs `nrm: vec3` to be a known Float anchor so the GLSL type
    // prefix resolves).
    for (const auto& s : vf->params) {
        if (auto* p = dynamic_cast<const phoskia::ir::IRShaderParam*>(s.get())) {
            const auto& info = tbl.at(p->semantic);
            const char* bgfxName = (p->dir == phoskia::ir::IRShaderParam::Direction::In)
                                       ? info.attrName : info.varyingName;
            vsCtx.map[p->name] = bgfxName;
            // Register both the Phoskia-side name and the bgfx-side
            // alias so let-initializer inference can find the type
            // regardless of which spelling a subsequent stmt uses.
            std::shared_ptr<phoskia::Type> t;
            const std::string& g = info.glslType;
            if      (g == "vec2") t = phoskia::BuiltinTypes::Vec2();
            else if (g == "vec3") t = phoskia::BuiltinTypes::Vec3();
            else if (g == "vec4") t = phoskia::BuiltinTypes::Vec4();
            if (t) {
                vsEnv.addVariable(p->name, t);
                vsEnv.addVariable(bgfxName, t);
            }
        }
    }
    for (const auto& s : ff->inputs) {
        if (auto* p = dynamic_cast<const phoskia::ir::IRShaderParam*>(s.get())) {
            const auto& info = tbl.at(p->semantic);
            fsCtx.map[p->name] = info.varyingName;
            std::shared_ptr<phoskia::Type> t;
            const std::string& g = info.glslType;
            if      (g == "vec2") t = phoskia::BuiltinTypes::Vec2();
            else if (g == "vec3") t = phoskia::BuiltinTypes::Vec3();
            else if (g == "vec4") t = phoskia::BuiltinTypes::Vec4();
            if (t) {
                fsEnv.addVariable(p->name, t);
                fsEnv.addVariable(info.varyingName, t);
            }
        }
    }

    // ---- varying.def.sc
    // bgfx canonical layout (matches examples/01-cubes/varying.def.sc):
    //   vec4 v_color0    : COLOR0    = vec4(1.0, 0.0, 0.0, 1.0);
    //
    //   vec3 a_position  : POSITION;
    //   vec4 a_color0    : COLOR0;
    // i.e. each line is "<type> <name>(pad to 11) : <semantic>(pad to 9) = <default>;"
    // or "<type> <name>(pad to 11) : <semantic>;" for plain attributes. The
    // 11/9 column widths match the bgfx examples; if a name or semantic
    // exceeds the column width the row naturally aligns further right
    // without breaking parsing.
    constexpr size_t kNameWidth = 12;
    constexpr size_t kSemanticWidth = 10;
    auto pad = [](const std::string& s, size_t w) {
        return s.size() < w ? s + std::string(w - s.size(), ' ') : s + " ";
    };
    std::ostringstream vdef;
    std::vector<phoskia::PhoskiaSemantic> writtenVaryings;
    auto writeVarying = [&](phoskia::PhoskiaSemantic sem) {
        for (auto s : writtenVaryings) if (s == sem) return;
        writtenVaryings.push_back(sem);
        const auto& info = tbl.at(sem);
        vdef << info.glslType << " " << pad(info.varyingName, kNameWidth)
             << ": " << pad(varyingSemantics.lookup(sem), kSemanticWidth)
             << "= " << info.defaultExpr << ";\n";
    };
    for (const auto& s : vf->params) {
        if (auto* p = dynamic_cast<const phoskia::ir::IRShaderParam*>(s.get())) {
            if (p->dir == phoskia::ir::IRShaderParam::Direction::Out) writeVarying(p->semantic);
        }
    }
    for (const auto& s : ff->inputs) {
        if (auto* p = dynamic_cast<const phoskia::ir::IRShaderParam*>(s.get())) {
            if (p->dir == phoskia::ir::IRShaderParam::Direction::In) writeVarying(p->semantic);
        }
    }
    for (const auto& s : vf->params) {
        if (auto* p = dynamic_cast<const phoskia::ir::IRShaderParam*>(s.get())) {
            if (p->dir == phoskia::ir::IRShaderParam::Direction::In) {
                const auto& info = tbl.at(p->semantic);
                vdef << info.glslType << " " << pad(info.attrName, kNameWidth)
                     << ": " << info.attrSemantic << ";\n";
            }
        }
    }

    // ---- vs_<Material>.sc
    std::ostringstream vs;
    {
        vs << "$input";
        bool first = true;
        for (const auto& s : vf->params) {
            if (auto* p = dynamic_cast<const phoskia::ir::IRShaderParam*>(s.get())) {
                if (p->dir != phoskia::ir::IRShaderParam::Direction::In) continue;
                const auto& info = tbl.at(p->semantic);
                vs << (first ? " " : ", ") << info.attrName;
                first = false;
            }
        }
        vs << "\n";
    }
    {
        vs << "$output";
        bool first = true;
        for (const auto& s : vf->params) {
            if (auto* p = dynamic_cast<const phoskia::ir::IRShaderParam*>(s.get())) {
                if (p->dir != phoskia::ir::IRShaderParam::Direction::Out) continue;
                const auto& info = tbl.at(p->semantic);
                vs << (first ? " " : ", ") << info.varyingName;
                first = false;
            }
        }
        vs << "\n";
    }
    vs << "\n#include \"common.sh\"\n\n"
       << _uboDecls
       << _uniformDecls
       << _propertyUniforms
       << "\nvoid main()\n{\n";
    for (const auto& s : vf->params) {
        if (auto* p = dynamic_cast<const phoskia::ir::IRShaderParam*>(s.get())) {
            if (p->dir != phoskia::ir::IRShaderParam::Direction::Out) {
                continue;
            }
            const auto& info = tbl.at(p->semantic);
            vs << "    " << info.varyingName << " = ";
            if (p->defaultValue) {
                emitExpr(vs, *p->defaultValue, vsCtx);
            } else {
                vs << info.defaultExpr;
            }
            vs << ";\n";
        }
    }
    {
        // #[variant name] in Phoskia source expands to
        //   #ifndef BGFX_VARIANT_<NAME>
        //       <skipped code>
        //   #else
        //       <actual code>
        //   #endif
        // — opt-in: shaderc must be invoked with `--define BGFX_VARIANT_<NAME>`
        // to enable the variant block. The `VariantAttribute` itself is NOT
        // emitted as text; it only toggles the conditional for the
        // statements that follow it within the same block.
        std::vector<std::string> openVariants;
        for (const auto& stmt : vf->body) {
            if (auto va = dynamic_cast<const phoskia::ir::IRVariantAttribute*>(stmt.get())) {
                vs << "#ifndef " << variantMacroName(va->name) << "\n";
                vs << "    // variant: skipped unless --define " << variantMacroName(va->name) << "\n";
                vs << "#else\n";
                openVariants.push_back(va->name);
            } else {
                emitStmt(vs, *stmt, vsCtx, "gl_Position", vsEnv);
            }
        }
        for (size_t i = 0; i < openVariants.size(); ++i) {
            (void)i;  // (no per-variant data needed; close them in order)
            vs << "#endif\n";
        }
    }
    vs << "}\n";

    // ---- fs_<Material>.sc
    std::ostringstream fs;
    {
        fs << "$input";
        bool first = true;
        for (const auto& s : ff->inputs) {
            if (auto* p = dynamic_cast<const phoskia::ir::IRShaderParam*>(s.get())) {
                if (p->dir != phoskia::ir::IRShaderParam::Direction::In) continue;
                const auto& info = tbl.at(p->semantic);
                fs << (first ? " " : ", ") << info.varyingName;
                first = false;
            }
        }
        fs << "\n";
    }
    fs << "\n#include \"common.sh\"\n\n"
       << _uboDecls
       << _uniformDecls
       << _propertyUniforms
       << _textureDecls
       << "\nvoid main()\n{\n";
    {
        // See vertex block above for the #[variant] semantics — opt-in
        // #ifndef that selects the variant code only when the user passes
        // `--define BGFX_VARIANT_<NAME>` to shaderc.
        std::vector<std::string> openVariants;
        for (const auto& stmt : ff->body) {
            if (auto va = dynamic_cast<const phoskia::ir::IRVariantAttribute*>(stmt.get())) {
                fs << "#ifndef " << variantMacroName(va->name) << "\n";
                fs << "    // variant: skipped unless --define " << variantMacroName(va->name) << "\n";
                fs << "#else\n";
                openVariants.push_back(va->name);
            } else {
                emitStmt(fs, *stmt, fsCtx, "gl_FragColor", fsEnv);
            }
        }
        for (size_t i = 0; i < openVariants.size(); ++i) {
            (void)i;
            fs << "#endif\n";
        }
    }
    fs << "}\n";

    detail::BGFXMaterialStages out{vs.str(), fs.str(), vdef.str()};

    _uniforms.insert(_uniforms.begin(), uniformSave.begin(), uniformSave.end());
    _textures.insert(_textures.begin(), textureSave.begin(), textureSave.end());
    return out;
}

// --------------------------------------------------------------------------
// Per-compute conversion
// --------------------------------------------------------------------------
//
// Phase 3.2: bgfx 1.18 + shaderc 1.18 fully support compute shaders. The
// `.sc` source for a compute shader is a single file (no vs/fs/varyingdef
// split) with the following shape:
//
//   $input                // empty — compute has no attributes
//   $output               // empty — compute has no varyings
//
//   #include "common.sh"
//
//   // (storage buffers / uniforms will go here in later blocks)
//
//   layout(local_size_x = 64) in;
//
//   void main() {
//       <body — IRComputeDecl::body>
//   }
//
// shaderc is invoked with `--type compute` to produce the .bin blob that
// `bgfx::createProgram(ShaderHandle _csh)` consumes.
//
// numthreads is fixed at 64 in this Phase 3.2 cut (Phoskia's surface
// syntax does not yet expose `[numthreads(X, Y, Z)] compute Foo { ... }`).
// The dispatch (X*Y*Z workgroups) is the engine's responsibility at the
// `bgfx::dispatch(_handle, X, Y, Z)` call site — Phoskia just emits the
// kernel.
//
// Phase 3.2 Block 1: body emission supports whatever `let / return /
// expression-statement / if / for` is in the IR — same `emitStmt`
// machinery as material bodies. The storage-buffer and thread-id
// extensions land in Blocks 2 and 3.

detail::BGFXComputeStage AYBGFXConverter::convertComputeDecl(const phoskia::ir::IRComputeDecl& compute) {
    // Compute uses its own per-call type env for let-stmt type
    // inference (mirrors material's vsEnv/fsEnv pattern). Compute
    // bodies are flat: no vs/fs split, no in/out param renames, no
    // output-var slot binding (compute return is early-exit, not an
    // output assignment). So the env starts empty and the rename
    // context is empty too.
    phoskia::TypeEnvironment env;
    RenameContext ctx;

    std::ostringstream cs;
    cs << "$input\n"           // empty input list (compute has no attributes)
       << "$output\n"          // empty output list (compute has no varyings)
       << "\n#include \"common.sh\"\n\n"
       << _uboDecls;            // Phase 3.4: top-level UBOs (GLSL allows the
                                // same block in multiple stages; pre-emitted
                                // by convertBGFX() before convertComputeDecl).

    // GLSL 4.30 / OpenGL ES 3.1 workgroup layout. bgfx's GLSL profile
    // accepts the `layout(local_size_x = N) in;` form (Y and Z default
    // to 1). shaderc validates this against the target profile; for
    // the linux/GLSL 120 target used by the e2e tests, compute needs
    // a higher profile — see Test_ShaderCompile.cpp where the target
    // is bumped for compute.
    //
    // Phase 3.3 Block 2: the user-supplied `[numthreads(X, Y, Z)]`
    // attribute (if present on the AST / IR node) takes priority. When
    // absent we fall back to the historical 64 default — kept for
    // backward compatibility with Phase 3.2 compute sources that don't
    // declare a workgroup shape.
    {
        uint32_t nx = 64, ny = 1, nz = 1;
        if (compute.hasNumThreads) {
            nx = compute.numThreads[0];
            ny = compute.numThreads[1];
            nz = compute.numThreads[2];
        }
        cs << "layout(local_size_x = " << nx
           << ", local_size_y = " << ny
           << ", local_size_z = " << nz << ") in;\n\n";
    }

    // Phase 3.2 Block 3: storage buffer declarations. Each Storage-kind
    // IRDeclaration becomes a GLSL `buffer` block. Both Read and
    // ReadWrite access forms share the same `buffer` syntax (GLSL
    // doesn't distinguish them at the source level — qualifiers live
    // on the type, not the block). The access field is preserved in IR
    // for future HLSL emitter use (Phase 5+) where Read maps to
    // `StructuredBuffer<T>` and ReadWrite maps to `RWStructuredBuffer<T>`.
    //
    // Element type: taken from `decl->storageElementType->toString()`,
    // which produces the GLSL lexeme ("float" / "vec3" / "ivec4" / ...).
    // When the IRGenerator couldn't resolve the element type (unknown
    // lexeme), it falls back to vec4 — same fallback as property
    // emission, kept consistent so storage reads / writes type-check.
    //
    // Phase 3.3 Block 4: Shared-kind declarations emit as GLSL
    // `shared T name[N];` (workgroup local memory). All threads in
    // the same workgroup see the same array; reads / writes from one
    // thread become visible to peers after a barrier (barrier syntax
    // is a separate extension; the memory itself is just declared
    // here).
    //
    // Phase 3.5-A: explicit binding slot (`layout(std430, binding = N)`).
    // Three sub-rules:
    //   1. If two storage decls share the same explicit binding slot,
    //      the conversion throws (caught by convertBGFX → reported in
    //      out.errors). This is a hard user error — same binding for
    //      two buffers means the runtime can't tell them apart.
    //   2. Decl without explicit binding (-1) gets auto-assigned the
    //      next free slot, starting from max(explicit bindings) + 1.
    //      This way an auto slot never collides with an explicit one.
    //   3. The resolved binding (explicit or auto) is recorded in
    //      `_storageBuffers` for the frontend to wire up at dispatch.
    if (!compute.declarations.empty()) {
        // (1) Duplicate-binding detection.
        std::unordered_map<int, std::string> usedBindings;
        for (const auto& decl : compute.declarations) {
            if (!decl || decl->kind != phoskia::ir::IRDeclaration::Kind::Storage) continue;
            if (decl->storageBinding < 0) continue;
            auto it = usedBindings.find(decl->storageBinding);
            if (it != usedBindings.end()) {
                throw std::runtime_error("Storage buffer '" + decl->name +
                    "' has duplicate binding " + std::to_string(decl->storageBinding) +
                    " (also used by '" + it->second + "')");
            }
            usedBindings[decl->storageBinding] = decl->name;
        }
        // (2) Compute the next free auto-binding slot.
        int nextAutoBinding = 0;
        for (const auto& decl : compute.declarations) {
            if (!decl || decl->kind != phoskia::ir::IRDeclaration::Kind::Storage) continue;
            if (decl->storageBinding >= 0 && decl->storageBinding >= nextAutoBinding) {
                nextAutoBinding = decl->storageBinding + 1;
            }
        }
        // (3) Emit each decl with its resolved binding.
        for (const auto& decl : compute.declarations) {
            if (!decl) continue;
            if (decl->kind == phoskia::ir::IRDeclaration::Kind::Storage) {
                std::string elementLex = "vec4";
                if (decl->storageElementType) {
                    elementLex = decl->storageElementType->toString();
                }
                int binding = decl->storageBinding;
                if (binding < 0) binding = nextAutoBinding++;
                // Phase 3.5-A: explicit binding → std430 layout
                // qualifier. std430 (not std140) matches GLSL's
                // storage-buffer layout rules — looser packing,
                // friendly to runtime-sized arrays.
                cs << "layout(std430, binding = " << binding << ") ";
                // GLSL storage buffer syntax:
                //   layout(std430, binding = N) buffer Name { Type data[]; } Name;
                // The trailing `Name;` (instance name) is required by
                // GLSL — the block's declared name and the instance
                // name can differ in principle but conventionally
                // match.
                cs << "buffer " << decl->name << " { "
                   << elementLex << " data[]; } "
                   << decl->name << ";\n";

                BGFXStorageBuffer bsb;
                bsb.name = decl->name;
                bsb.binding = binding;
                bsb.elementType = elementLex;
                _storageBuffers.push_back(std::move(bsb));
            } else if (decl->kind == phoskia::ir::IRDeclaration::Kind::Shared) {
                std::string elementLex = "vec4";
                if (decl->sharedElementType) {
                    elementLex = decl->sharedElementType->toString();
                }
                // GLSL workgroup-shared array:
                //   shared T name[N];
                // The size is a compile-time constant int (the parser
                // already enforced that). bgfx's GLSL profile accepts the
                // standard GLSL form; HLSL would need `groupshared` (a
                // Phase 5+ HLSL emitter concern).
                cs << "shared " << elementLex << " " << decl->name
                   << "[" << decl->sharedSize << "];\n";
            }
            // Other decl kinds are silently skipped here — compute
            // uniforms / properties land in a future block.
        }
        cs << "\n";
    }

    // Body — same emitStmt machinery as material bodies, but with
    // outputVar=nullptr (no implicit output slot binding; return is
    // early-exit only).
    cs << "void main()\n{\n";
    for (const auto& stmt : compute.body) {
        if (!stmt) continue;
        // Compute bodies do not currently support [variant] (they have
        // no in-shader opt-in semantics that make sense for a kernel).
        // If an IRVariantAttribute ever appears here, skip it silently
        // rather than recursing — emitStmt has no path for it. Future
        // work: add a kernel-level variant mechanism (e.g. per-dispatch
        // defines) if needed.
        if (dynamic_cast<const phoskia::ir::IRVariantAttribute*>(stmt.get())) continue;
        emitStmt(cs, *stmt, ctx, nullptr, env);
    }
    cs << "}\n";

    detail::BGFXComputeStage out;
    out.compute = cs.str();
    return out;
}

void AYBGFXConverter::generateProperty(const phoskia::ir::IRDeclaration& decl) {
    // Used only when called outside convertMaterial; convertMaterial does
    // its own property emission via emitPropertyUniform().
    std::ostringstream tmp;
    emitPropertyUniform(tmp, decl);
    _propertyUniforms += tmp.str();
    BGFXUniform bu; bu.name = decl.name; bu.type = inferPropertyGLSLType(decl);
    _uniforms.push_back(std::move(bu));
}

void AYBGFXConverter::generateExpr(const phoskia::ir::IRExpr& expr) {
    std::ostringstream tmp;
    RenameContext empty;
    emitExpr(tmp, expr, empty);
    _uniformDecls += tmp.str();  // best-effort scratch, not actually used
}

} // namespace ayt::shader
