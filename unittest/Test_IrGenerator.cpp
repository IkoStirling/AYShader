// ============================================================
// AYShader IR Generator Unit Tests (Phase 3.1)
// ============================================================
//
// Exercises IRGenerator::generate — the AST → IR lowering pass. Every
// IR expression carries a pre-resolved `Type` pointer; backends read it
// directly instead of running TypeInference. The IR is a 1:1 mirror of
// the AST with the same node classes plus resolvedType on every Expr.
//
// Test surface (5 cases):
//   - ir_minimal_material_lowers_to_ir_program
//       IR separates declarations + vertex/fragment into pre-sorted fields.
//   - ir_let_stmt_carries_resolved_type
//       let x = 1.0 → IRLetStmt.initializer->resolvedType == Float.
//   - ir_call_expr_carries_resolved_type
//       normalize(vec3(1.0)) → IRCallExpr.resolvedType == Vec3.
//   - ir_property_uniform_type_resolved_from_initializer
//       property col = vec3(1.0) → IRDeclaration.propertyInit type is Vec3,
//       and the BGFX backend emits `uniform vec3 col` (the regression
//       fixed in b9723a5 must survive the IR transition).
//   - ir_compute_decl_passes_through_faithfully
//       compute Name { body } lowers to IRComputeDecl with the body intact.

#include "AYIr.h"
#include "AYLexer.h"
#include "AYParser.h"
#include "AYAst.h"
#include "AYType.h"
#include "AYBGFXConverter.h"
#include "AYTest.h"
#include <memory>
#include <string>

using namespace ayt::shader::phoskia;
using ayt::shader::phoskia::ir::IRProgram;
using ayt::shader::phoskia::ir::IRGenerator;
using ayt::shader::phoskia::ir::IRMaterialDecl;
using ayt::shader::phoskia::ir::IRComputeDecl;
using ayt::shader::phoskia::ir::IRLetStmt;
using ayt::shader::phoskia::ir::IRCallExpr;
using ayt::shader::phoskia::ir::IRDeclaration;

TEST_SUITE(IrGeneratorTests)

// ===== Helpers =====

static std::unique_ptr<Program> parseProgram(const std::string& src) {
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    return parser.parse();
}

static IRProgram generateIR(const std::string& src) {
    auto ast = parseProgram(src);
    IRGenerator gen;
    return gen.generate(*ast);
}

// ===== Tests =====

TEST_CASE(ir_minimal_material_lowers_to_ir_program) {
    auto ir = generateIR(R"(
        material P {
            vertex { }
            fragment { }
        }
    )");
    CHECK(ir.materials.size() == 1);
    CHECK(ir.computes.empty());
    const auto& mat = ir.materials.front();
    CHECK(mat->name == "P");
    CHECK(mat->vertex != nullptr);
    CHECK(mat->fragment != nullptr);
    CHECK(mat->vertex->body.empty());
    CHECK(mat->fragment->body.empty());
}

TEST_CASE(ir_let_stmt_carries_resolved_type) {
    auto ir = generateIR(R"(
        material P {
            vertex { let x = 1.0; return vec4(x) }
            fragment { return vec4(1.0) }
        }
    )");
    const auto& mat = ir.materials.front();
    CHECK_NOT_NULL(mat->vertex.get());
    // The vertex body has exactly one LetStmt (the `return` is a ReturnStmt).
    bool foundLet = false;
    for (const auto& s : mat->vertex->body) {
        if (auto let = dynamic_cast<IRLetStmt*>(s.get())) {
            foundLet = true;
            CHECK(let->name == "x");
            CHECK_NOT_NULL(let->initializer.get());
            CHECK_NOT_NULL(let->initializer->resolvedType.get());
            // 1.0 is a Float literal — should resolve to PrimitiveType(Float).
            auto prim = std::dynamic_pointer_cast<PrimitiveType_>(let->initializer->resolvedType);
            CHECK_NOT_NULL(prim.get());
            CHECK(prim->primitive() == PrimitiveType::Float);
        }
    }
    CHECK(foundLet);
}

TEST_CASE(ir_call_expr_carries_resolved_type) {
    auto ir = generateIR(R"(
        material P {
            vertex { let y = normalize(vec3(1.0, 0.0, 0.0)); return vec4(y, 1.0) }
            fragment { return vec4(1.0) }
        }
    )");
    const auto& mat = ir.materials.front();
    CHECK_NOT_NULL(mat->vertex.get());
    bool foundLet = false;
    for (const auto& s : mat->vertex->body) {
        if (auto let = dynamic_cast<IRLetStmt*>(s.get())) {
            foundLet = true;
            CHECK_NOT_NULL(let->initializer.get());
            // normalize(vec3(...)) is a builtin call → resolvedType is
            // VectorType(Float, 3) per the BuiltinFunctionRegistry.
            auto vec = std::dynamic_pointer_cast<VectorType>(let->initializer->resolvedType);
            CHECK_NOT_NULL(vec.get());
            CHECK(vec->dimension() == 3);
            CHECK(vec->elementType() == PrimitiveType::Float);
        }
    }
    CHECK(foundLet);
}

TEST_CASE(ir_property_uniform_type_resolved_from_initializer) {
    // The original bug fixed in commit b9723a5 was that the BGFX
    // converter hardcoded `uniform vec4 col` regardless of the
    // initializer's actual type. After IR-based type resolution, the
    // property initializer's resolvedType drives the uniform declaration
    // and the output must match the initializer shape (`uniform vec3 col`).
    auto ir = generateIR(R"(
        material P {
            property col = vec3(1.0, 0.0, 0.0)
            vertex { }
            fragment { return vec4(col, 1.0) }
        }
    )");
    const auto& mat = ir.materials.front();
    CHECK_NOT_NULL(mat->vertex.get());

    // Find the property declaration.
    bool foundProp = false;
    for (const auto& decl : mat->declarations) {
        if (decl->kind == IRDeclaration::Kind::Property && decl->name == "col") {
            foundProp = true;
            CHECK_NOT_NULL(decl->propertyInit.get());
            auto vec = std::dynamic_pointer_cast<VectorType>(decl->propertyInit->resolvedType);
            CHECK_NOT_NULL(vec.get());
            CHECK(vec->dimension() == 3);
        }
    }
    CHECK(foundProp);

    // End-to-end: confirm the BGFX backend emits `uniform vec3 col`
    // (NOT the legacy hardcoded `uniform vec4 col`).
    Lexer lexer(R"(
        material P {
            property col = vec3(1.0, 0.0, 0.0)
            vertex { }
            fragment { return vec4(col, 1.0) }
        }
    )");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto ast = parser.parse();
    IRGenerator gen;
    IRProgram program = gen.generate(*ast);

    ayt::shader::AYBGFXConverter conv;
    ayt::shader::BGFXConvertResult bgfx;
    conv.convertBGFX(program, bgfx);
    CHECK(bgfx.success);
    CHECK_FALSE(bgfx.materialFiles.empty());
    CHECK(bgfx.materialFiles.front().vs.find("uniform vec3 col") != std::string::npos);
}

TEST_CASE(ir_compute_decl_passes_through_faithfully) {
    auto ir = generateIR(R"(
        compute Foo {
            let idx = 0
            return idx
        }
        material P {
            vertex { }
            fragment { }
        }
    )");
    CHECK(ir.computes.size() == 1);
    CHECK(ir.materials.size() == 1);
    const auto& cmp = ir.computes.front();
    CHECK(cmp->name == "Foo");
    CHECK(cmp->body.size() == 2);  // let + return
    CHECK(dynamic_cast<IRLetStmt*>(cmp->body[0].get()) != nullptr);
}

// ===== Phase 3.3 Block 3: uvec3 strict typing on thread_id =====
TEST_CASE(ir_thread_id_x_resolves_to_uint) {
    // `thread_id` is registered as a 0-arg builtin returning uvec3.
    // The bare-identifier form (`thread_id.x` rather than
    // `thread_id().x`) reaches `inferMemberExpr` with objectType =
    // uvec3, and the single-axis swizzle resolves to `uint`. The
    // BGFX backend's let-stmt emission then reads that resolvedType
    // and emits `uint idx = ...` — strict GLSL type-correctness.
    auto ir = generateIR(R"(
        compute Foo {
            let idx = thread_id.x
            return idx
        }
    )");
    CHECK(ir.computes.size() == 1);
    const auto& cmp = ir.computes.front();
    CHECK(cmp->body.size() == 2);
    auto* let = dynamic_cast<IRLetStmt*>(cmp->body[0].get());
    CHECK_NOT_NULL(let);
    auto prim = std::dynamic_pointer_cast<PrimitiveType_>(let->initializer->resolvedType);
    CHECK_NOT_NULL(prim.get());
    CHECK(prim->primitive() == PrimitiveType::Uint);
}

}