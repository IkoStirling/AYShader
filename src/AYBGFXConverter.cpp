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
#include "AYAst.h"
#include "AYType.h"
#include "AYTypeInference.h"  // Phase 2: let stmt needs GLSL type prefix.
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

// PhoskiaSemantic → bgfx semantic binding table. bgfx examples use these
// exact mappings (POSITION/NORMAL/COLOR0/TEXCOORD0) for the first slot of
// each kind; the converter writes varying.def.sc from this table.
//
// Phase 1: each semantic appears at most once per block. Phase 2 will
// introduce texcoord0..7 / color0..1 / tangent / binormal.
struct PhoskiaSemanticInfo {
    const char* bgfxSemantic;   // POSITION / NORMAL / COLOR0 / TEXCOORD0
    const char* attrName;       // a_position / a_normal / ...
    const char* varyingName;    // v_position / v_normal / ...
    const char* glslType;       // vec3 / vec4 / vec2
    const char* defaultExpr;    // vec3(0.0, 0.0, 0.0) etc.
};

const std::unordered_map<phoskia::PhoskiaSemantic, PhoskiaSemanticInfo>&
semanticTable() {
    static const std::unordered_map<phoskia::PhoskiaSemantic, PhoskiaSemanticInfo> table = {
        {phoskia::PhoskiaSemantic::Position, {"POSITION",  "a_position",  "v_position",  "vec3", "vec3(0.0, 0.0, 0.0)"}},
        {phoskia::PhoskiaSemantic::Normal,   {"NORMAL",    "a_normal",    "v_normal",    "vec3", "vec3(0.0, 0.0, 1.0)"}},
        {phoskia::PhoskiaSemantic::Color,    {"COLOR0",    "a_color0",    "v_color0",    "vec4", "vec4(1.0, 0.0, 0.0, 1.0)"}},
        {phoskia::PhoskiaSemantic::Texcoord, {"TEXCOORD0", "a_texcoord0", "v_texcoord0", "vec2", "vec2(0.0, 0.0)"}},
    };
    return table;
}

// Forward decl of helper used by emit* functions.
void emitExpr(std::ostringstream& out, const phoskia::Expr& e);

// RenameContext maps a Phoskia identifier to its bgfx-side counterpart.
// Built per-shader-block from the in/out ShaderParam declarations: the
// Phoskia name (`pos`, `uv`, ...) becomes the bgfx attr/varying name
// (`a_position`, `v_texcoord0`, ...). The converter passes the context
// into emitExpr so identifiers are rewritten on the fly — Phoskia source
// stays bgfx-agnostic.
struct RenameContext {
    std::unordered_map<std::string, std::string> map;
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

void emitExpr(std::ostringstream& out, const phoskia::Expr& e,
              const RenameContext& ctx);

void emitStmt(std::ostringstream& out, const phoskia::Stmt& s,
              const RenameContext& ctx, const char* outputVar,
              phoskia::TypeEnvironment& env);

void emitStmt(std::ostringstream& out, const phoskia::Stmt& s,
              const RenameContext& ctx, phoskia::TypeEnvironment& env) {
    emitStmt(out, s, ctx, nullptr, env);
}

void emitStmt(std::ostringstream& out, const phoskia::Stmt& s,
              const RenameContext& ctx) {
    // Forward-compat overload retained only for the property / texture
    // emitExpr paths which don't participate in the let-stmt type
    // inference. Always uses a throwaway env.
    phoskia::TypeEnvironment throwaway;
    emitStmt(out, s, ctx, nullptr, throwaway);
}

void emitStmt(std::ostringstream& out, const phoskia::Stmt& s,
              const RenameContext& ctx, const char* outputVar,
              phoskia::TypeEnvironment& env) {
    if (auto let = dynamic_cast<const phoskia::LetStmt*>(&s)) {
        // GLSL (and bgfx's GLSL profile) requires an explicit type on
        // every local declaration. We run the type-inference engine on
        // the initializer to recover the right GLSL type, then register
        // the binding in `env` so later statements that reference
        // `let->name` see a concrete type instead of a fresh TypeVar
        // (chain-of-arithmetic propagation). Phase 2 Step 2 built that
        // engine; Step 5 made builtin constructors (vec3 / mat4)
        // resolve through it.
        std::string glslType;
        std::shared_ptr<phoskia::Type> resolvedType;
        if (let->initializer) {
            phoskia::TypeInference inference(env);
            auto inferred = inference.infer(*let->initializer);
            std::shared_ptr<phoskia::Type> concrete = inferred;
            while (auto tv = std::dynamic_pointer_cast<phoskia::TypeVar>(concrete)) {
                if (tv->hasSolution()) concrete = tv->getSolution();
                else break;
            }
            resolvedType = concrete;
            if (auto vec = std::dynamic_pointer_cast<phoskia::VectorType>(concrete)) {
                glslType = vec->toString();  // "vec2" / "vec3" / "vec4" / "ivec3" / ...
            } else if (auto mat = std::dynamic_pointer_cast<phoskia::MatrixType>(concrete)) {
                glslType = mat->toString();  // "mat2" / "mat3" / "mat4"
            } else if (auto p = std::dynamic_pointer_cast<phoskia::PrimitiveType_>(concrete)) {
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
    } else if (auto ret = dynamic_cast<const phoskia::ReturnStmt*>(&s)) {
        out << "    ";
        // Phoskia source: `return <vec4-expr>`. The compiler implicitly
        // binds the return value to the block's output slot — `gl_Position`
        // for vertex, `gl_FragColor` for fragment. User source never
        // references these identifiers directly.
        if (outputVar) out << outputVar << " = ";
        if (ret->value) emitExpr(out, *ret->value, ctx);
        out << ";\n";
    } else if (auto es = dynamic_cast<const phoskia::ExprStmt*>(&s)) {
        if (es->expr) {
            out << "    ";
            emitExpr(out, *es->expr, ctx);
            out << ";\n";
        }
    }
    // ShaderParam / IfStmt / ForStmt at the body level should not appear
    // (they live in the params/inputs vector before body). Ignore if seen.
}

void emitExpr(std::ostringstream& out, const phoskia::Expr& e,
              const RenameContext& ctx) {
    if (auto bin = dynamic_cast<const phoskia::BinaryExpr*>(&e)) {
        // Parenthesize the whole subexpression so operator precedence
        // is preserved in the emitted GLSL. Without the parens, a
        // chain like `a * (b - c) * d` (parsed as `((a * (b-c)) * d)`)
        // would emit as `a * b - c * d` and silently change the math
        // under GLSL's left-associative precedence rules. Wrapping
        // every BinaryExpr in (...) is verbose but always correct.
        out << "(";
        emitExpr(out, *bin->left, ctx);
        out << " " << bin->op.lexeme << " ";
        emitExpr(out, *bin->right, ctx);
        out << ")";
    } else if (auto un = dynamic_cast<const phoskia::UnaryExpr*>(&e)) {
        out << un->op.lexeme;
        emitExpr(out, *un->operand, ctx);
    } else if (auto call = dynamic_cast<const phoskia::CallExpr*>(&e)) {
        // Builtin mapping: Phoskia's `sample(tex, uv)` → bgfx `texture2D(tex, uv)`.
        // PBR math functions (fresnelSchlick / distributionGGX /
        // geometrySchlickGGX / geometrySmith) registered as Phase 2
        // Step 3 builtins are inlined to plain GLSL expressions here
        // because bgfx's shaderc does not recognise the Phoskia
        // names. The TypeInference engine knows about these
        // signatures for type-checking; the converter handles
        // emission. After inline, the result is the same GLSL a
        // user would have written by hand from the math.
        if (auto callee = dynamic_cast<const phoskia::IdentifierExpr*>(call->callee.get())) {
            if (callee->name == "sample") {
                out << "texture2D";
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
                    auto emitGSub = [&](const phoskia::Expr& ndot) {
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
    } else if (auto ident = dynamic_cast<const phoskia::IdentifierExpr*>(&e)) {
        // Apply rename map for in-param identifiers (Phoskia → bgfx).
        out << ctx.lookup(ident->name);
    } else if (auto lit = dynamic_cast<const phoskia::LiteralExpr*>(&e)) {
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
    } else if (auto mem = dynamic_cast<const phoskia::MemberExpr*>(&e)) {
        emitExpr(out, *mem->object, ctx);
        out << "." << mem->member;
    } else if (auto idx = dynamic_cast<const phoskia::IndexExpr*>(&e)) {
        emitExpr(out, *idx->object, ctx);
        out << "[";
        emitExpr(out, *idx->index, ctx);
        out << "]";
    }
}

const phoskia::ShaderParam* findInParam(const std::vector<phoskia::StmtPtr>& params,
                                        phoskia::PhoskiaSemantic sem) {
    for (const auto& s : params) {
        auto* p = dynamic_cast<const phoskia::ShaderParam*>(s.get());
        if (p && p->dir == phoskia::ShaderParam::Direction::In && p->semantic == sem) {
            return p;
        }
    }
    return nullptr;
}

const phoskia::ShaderParam* findOutParam(const std::vector<phoskia::StmtPtr>& params,
                                         phoskia::PhoskiaSemantic sem) {
    for (const auto& s : params) {
        auto* p = dynamic_cast<const phoskia::ShaderParam*>(s.get());
        if (p && p->dir == phoskia::ShaderParam::Direction::Out && p->semantic == sem) {
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
// Run the type-inference engine on the initializer to recover the
// right type. shaderc (GLSL 1.20 in particular) rejects uniforms
// whose declared type doesn't match the initializer type with
// `initializer of type T cannot be assigned to variable of type U`,
// so this must match exactly.
std::string inferPropertyGLSLType(const phoskia::PropertyDecl& prop) {
    std::string glslType = "vec4";
    if (prop.initializer) {
        phoskia::TypeEnvironment env;  // empty: properties don't see body lets
        phoskia::TypeInference inference(env);
        auto inferred = inference.infer(*prop.initializer);
        std::shared_ptr<phoskia::Type> concrete = inferred;
        while (auto tv = std::dynamic_pointer_cast<phoskia::TypeVar>(concrete)) {
            if (tv->hasSolution()) concrete = tv->getSolution();
            else break;
        }
        if (auto vec = std::dynamic_pointer_cast<phoskia::VectorType>(concrete)) {
            glslType = vec->toString();
        } else if (auto mat = std::dynamic_pointer_cast<phoskia::MatrixType>(concrete)) {
            glslType = mat->toString();
        } else if (auto p = std::dynamic_pointer_cast<phoskia::PrimitiveType_>(concrete)) {
            glslType = p->toString();
        }
    }
    return glslType;
}

void emitPropertyUniform(std::ostream& out, const phoskia::PropertyDecl& prop) {
    std::string glslType = inferPropertyGLSLType(prop);
    out << "uniform " << glslType << " " << prop.name << " = ";
    if (prop.initializer) {
        std::ostringstream tmp;
        RenameContext empty;  // properties don't reference in/out params
        emitExpr(tmp, *prop.initializer, empty);
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

BGFXConvertResult AYBGFXConverter::convertBGFX(const phoskia::Program& ast) {
    BGFXConvertResult result;
    result.success = true;

    try {
        for (const auto& decl : ast.declarations) {
            if (auto mat = dynamic_cast<const phoskia::MaterialDecl*>(decl.get())) {
                result.materialFiles.push_back(convertMaterial(*mat));
            }
        }
    } catch (const std::exception& e) {
        result.errors.push_back(e.what());
        result.success = false;
    } catch (...) {
        result.errors.push_back("Unknown exception during BGFX conversion");
        result.success = false;
    }

    result.uniforms = _uniforms;
    result.textures = _textures;
    return result;
}

ConvertResult AYBGFXConverter::convert(const phoskia::Program& ast) {
    ConvertResult result;
    auto bgfx = convertBGFX(ast);
    result.success = bgfx.success;
    result.errors = bgfx.errors;
    std::ostringstream oss;
    for (size_t i = 0; i < bgfx.materialFiles.size(); ++i) {
        const auto& f = bgfx.materialFiles[i];
        oss << "// === material " << i << " varying.def.sc ===\n"
            << f.varyingDef
            << "\n// === material " << i << " vs ===\n"
            << f.vs
            << "\n// === material " << i << " fs ===\n"
            << f.fs
            << "\n";
    }
    result.output = oss.str();
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

BGFXShaderFiles AYBGFXConverter::convertMaterial(const phoskia::MaterialDecl& mat) {
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
    RenameContext vsCtx;
    RenameContext fsCtx;

    // First pass: material-level declarations.
    const phoskia::VertexFunc*   vf = nullptr;
    const phoskia::FragmentFunc* ff = nullptr;
    for (const auto& d : mat.declarations) {
        if (auto u = dynamic_cast<const phoskia::UniformDecl*>(d.get())) {
            _uniformDecls += "uniform " + u->type + " " + u->name + ";\n";
            BGFXUniform bu; bu.name = u->name; bu.type = u->type;
            _uniforms.push_back(std::move(bu));
            // Register uniform type into both envs so the let-stmt
            // type inference in either block resolves `roughness` to
            // float (or whichever GLSL type `u->type` maps to).
            auto t = phoskiaGLSLTypeToPhoskiaType(u->type);
            if (t) {
                vsEnv.addVariable(u->name, t);
                fsEnv.addVariable(u->name, t);
            }
        } else if (auto t = dynamic_cast<const phoskia::TextureDecl*>(d.get())) {
            uint8_t slot = static_cast<uint8_t>(_textures.size());
            _textureDecls += "SAMPLER2D(" + t->name + ", " + std::to_string(slot) + ");\n";
            BGFXTexture bt; bt.name = t->name; bt.binding = slot;
            _textures.push_back(std::move(bt));
            // Textures are opaque to the type system (Phase 2 Step 2
            // deferred TextureType); register as Dynamic so lookup
            // succeeds even though sample() body-side checks pass
            // through the builtin registry directly.
            vsEnv.addVariable(t->name, phoskia::BuiltinTypes::Dynamic);
            fsEnv.addVariable(t->name, phoskia::BuiltinTypes::Dynamic);
        } else if (auto p = dynamic_cast<const phoskia::PropertyDecl*>(d.get())) {
            std::ostringstream tmp;
            emitPropertyUniform(tmp, *p);
            _propertyUniforms += tmp.str();
            // The uniform type matches the GLSL type we emit — driven
            // by the initializer's inferred type so e.g. `property
            // emission = vec3(0.0)` becomes `uniform vec3 emission`.
            std::string glslType = inferPropertyGLSLType(*p);
            BGFXUniform bu; bu.name = p->name; bu.type = glslType;
            _uniforms.push_back(std::move(bu));
            // Register in both per-block type envs so the let-stmt
            // type inference in either block resolves references to
            // this property to the right GLSL type.
            std::shared_ptr<phoskia::Type> ptype;
            if      (glslType == "vec2") ptype = phoskia::BuiltinTypes::Vec2();
            else if (glslType == "vec3") ptype = phoskia::BuiltinTypes::Vec3();
            else if (glslType == "vec4") ptype = phoskia::BuiltinTypes::Vec4();
            if (ptype) {
                vsEnv.addVariable(p->name, ptype);
                fsEnv.addVariable(p->name, ptype);
            }
        } else if (auto v = dynamic_cast<const phoskia::VertexFunc*>(d.get())) {
            vf = v;
        } else if (auto f = dynamic_cast<const phoskia::FragmentFunc*>(d.get())) {
            ff = f;
        }
        // VariantAttribute at material level: ignored (variants only
        // appear inside vertex/fragment blocks, where they're consumed
        // by the per-block body loops below).
    }

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

    // Populate the rename maps + already-constructed per-block
    // TypeEnvironments with in/out params (e.g. `let N = normalize(nrm)`
    // needs `nrm: vec3` to be a known Float anchor so the GLSL type
    // prefix resolves).
    for (const auto& s : vf->params) {
        if (auto* p = dynamic_cast<const phoskia::ShaderParam*>(s.get())) {
            const auto& info = tbl.at(p->semantic);
            const char* bgfxName = (p->dir == phoskia::ShaderParam::Direction::In)
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
        if (auto* p = dynamic_cast<const phoskia::ShaderParam*>(s.get())) {
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
             << ": " << pad(info.bgfxSemantic, kSemanticWidth)
             << "= " << info.defaultExpr << ";\n";
    };
    for (const auto& s : vf->params) {
        if (auto* p = dynamic_cast<const phoskia::ShaderParam*>(s.get())) {
            if (p->dir == phoskia::ShaderParam::Direction::Out) writeVarying(p->semantic);
        }
    }
    for (const auto& s : ff->inputs) {
        if (auto* p = dynamic_cast<const phoskia::ShaderParam*>(s.get())) {
            if (p->dir == phoskia::ShaderParam::Direction::In) writeVarying(p->semantic);
        }
    }
    for (const auto& s : vf->params) {
        if (auto* p = dynamic_cast<const phoskia::ShaderParam*>(s.get())) {
            if (p->dir == phoskia::ShaderParam::Direction::In) {
                const auto& info = tbl.at(p->semantic);
                vdef << info.glslType << " " << pad(info.attrName, kNameWidth)
                     << ": " << info.bgfxSemantic << ";\n";
            }
        }
    }

    // ---- vs_<Material>.sc
    std::ostringstream vs;
    {
        vs << "$input";
        bool first = true;
        for (const auto& s : vf->params) {
            if (auto* p = dynamic_cast<const phoskia::ShaderParam*>(s.get())) {
                if (p->dir != phoskia::ShaderParam::Direction::In) continue;
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
            if (auto* p = dynamic_cast<const phoskia::ShaderParam*>(s.get())) {
                if (p->dir != phoskia::ShaderParam::Direction::Out) continue;
                const auto& info = tbl.at(p->semantic);
                vs << (first ? " " : ", ") << info.varyingName;
                first = false;
            }
        }
        vs << "\n";
    }
    vs << "\n#include \"common.sh\"\n\n"
       << _uniformDecls
       << _propertyUniforms
       << "\nvoid main()\n{\n";
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
            if (auto va = dynamic_cast<const phoskia::VariantAttribute*>(stmt.get())) {
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
            if (auto* p = dynamic_cast<const phoskia::ShaderParam*>(s.get())) {
                if (p->dir != phoskia::ShaderParam::Direction::In) continue;
                const auto& info = tbl.at(p->semantic);
                fs << (first ? " " : ", ") << info.varyingName;
                first = false;
            }
        }
        fs << "\n";
    }
    fs << "\n#include \"common.sh\"\n\n"
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
            if (auto va = dynamic_cast<const phoskia::VariantAttribute*>(stmt.get())) {
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

    BGFXShaderFiles out{vs.str(), fs.str(), vdef.str()};

    _uniforms.insert(_uniforms.begin(), uniformSave.begin(), uniformSave.end());
    _textures.insert(_textures.begin(), textureSave.begin(), textureSave.end());
    return out;
}

void AYBGFXConverter::generateProperty(const phoskia::PropertyDecl& prop) {
    // Used only when called outside convertMaterial; convertMaterial does
    // its own property emission via emitPropertyUniform().
    std::ostringstream tmp;
    emitPropertyUniform(tmp, prop);
    _propertyUniforms += tmp.str();
    BGFXUniform bu; bu.name = prop.name; bu.type = inferPropertyGLSLType(prop);
    _uniforms.push_back(std::move(bu));
}

void AYBGFXConverter::generateExpr(const phoskia::Expr& expr) {
    std::ostringstream tmp;
    RenameContext empty;
    emitExpr(tmp, expr, empty);
    _uniformDecls += tmp.str();  // best-effort scratch, not actually used
}

} // namespace ayt::shader
