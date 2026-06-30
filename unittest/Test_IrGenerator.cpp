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

// ===== Phase 3.3 Block 4: workgroup-shared local memory =====
TEST_CASE(ir_compute_shared_decl_lowers_to_shared_kind) {
    auto ir = generateIR(R"(
        compute Reduce {
            shared float tile[64]
            let i = thread_id.x
            tile[i] = i
            return tile[0]
        }
    )");
    CHECK(ir.computes.size() == 1);
    const auto& cmp = ir.computes.front();
    // The SharedDecl is in the IR's declarations vector (not in body)
    // because the BGFX converter emits shared arrays as
    // top-of-compute declarations, not inside main().
    CHECK(cmp->declarations.size() == 1);
    const auto& decl = cmp->declarations.front();
    CHECK_NOT_NULL(decl.get());
    CHECK(decl->kind == IRDeclaration::Kind::Shared);
    CHECK(decl->name == "tile");
    CHECK(decl->sharedSize == 64);
    auto vec = std::dynamic_pointer_cast<VectorType>(decl->sharedElementType);
    // sharedElementType for `shared float tile[64]` is
    // PrimitiveType_(Float) (the scalar), not VectorType — same
    // shape as StorageDecl's element type.
    auto prim = std::dynamic_pointer_cast<PrimitiveType_>(decl->sharedElementType);
    CHECK_NOT_NULL(prim.get());
    CHECK(prim->primitive() == PrimitiveType::Float);
}

// ===== Phase 3.4: uniform block (UBO) =====
TEST_CASE(ir_uniformblock_lowers_with_field_types) {
    // `uniformblock Camera { vec3 position; float fov; uint flags; }`
    // produces an IRProgram::uniformBlocks entry with one IRDeclaration
    // whose kind=UniformBlock. Field types resolve through the
    // lexemeToType table — vec3 → VectorType(Float,3), float →
    // Float, uint → Uint (Phase 3.3 Block 1).
    auto ir = generateIR(R"(
        uniformblock Camera {
            vec3 position
            float fov
            uint flags
        }
        material P { vertex { } fragment { } }
    )");
    CHECK(ir.uniformBlocks.size() == 1);
    const auto& decl = ir.uniformBlocks.front();
    CHECK_NOT_NULL(decl.get());
    CHECK(decl->kind == IRDeclaration::Kind::UniformBlock);
    CHECK(decl->name == "Camera");
    CHECK(decl->uboFields.size() == 3);
    CHECK(decl->uboFieldNames.size() == 3);
    CHECK(decl->uboFieldNames[0] == "position");
    CHECK(decl->uboFieldNames[1] == "fov");
    CHECK(decl->uboFieldNames[2] == "flags");
    // vec3 → VectorType(Float, 3)
    auto vec3 = std::dynamic_pointer_cast<VectorType>(decl->uboFields[0]);
    CHECK_NOT_NULL(vec3.get());
    CHECK(vec3->elementType() == PrimitiveType::Float);
    CHECK(vec3->dimension() == 3);
    // float → Float
    auto flt = std::dynamic_pointer_cast<PrimitiveType_>(decl->uboFields[1]);
    CHECK_NOT_NULL(flt.get());
    CHECK(flt->primitive() == PrimitiveType::Float);
    // uint → Uint
    auto u = std::dynamic_pointer_cast<PrimitiveType_>(decl->uboFields[2]);
    CHECK_NOT_NULL(u.get());
    CHECK(u->primitive() == PrimitiveType::Uint);
}

TEST_CASE(ir_uniformblock_binding_auto_increments) {
    // Two UBOs in declaration order: Camera gets binding 0, Lighting
    // gets binding 1. The IRGenerator's nextBinding_ counter advances
    // once per UBO at program-root scope; reset to 0 per generate()
    // call so the numbers are stable across re-runs.
    auto ir = generateIR(R"(
        uniformblock Camera {
            vec3 position
        }
        uniformblock Lighting {
            vec3 ambient
        }
        material P { vertex { } fragment { } }
    )");
    CHECK(ir.uniformBlocks.size() == 2);
    CHECK(ir.uniformBlocks[0]->name == "Camera");
    CHECK(ir.uniformBlocks[0]->uboBinding == 0);
    CHECK(ir.uniformBlocks[1]->name == "Lighting");
    CHECK(ir.uniformBlocks[1]->uboBinding == 1);
}

TEST_CASE(ir_uniformblock_unknown_field_type_warns) {
    // An unrecognised field-type lexeme (e.g. `struct Particle`)
    // produces a non-fatal warning and falls back to vec4. We
    // can't check the warning string here (it's only collected on
    // IRProgram::warnings which the test framework doesn't print
    // by default), but the field is still lowered — the block
    // doesn't error out and just emits `vec4 particle;` in GLSL.
    auto ir = generateIR(R"(
        uniformblock Foo {
            Particle particle
        }
    )");
    CHECK(ir.uniformBlocks.size() == 1);
    auto vec4 = std::dynamic_pointer_cast<VectorType>(ir.uniformBlocks.front()->uboFields[0]);
    CHECK_NOT_NULL(vec4.get());
    CHECK(vec4->elementType() == PrimitiveType::Float);
    CHECK(vec4->dimension() == 4);
    CHECK_FALSE(ir.warnings.empty());  // at least one warning
}

// ===== Phase 3.5-A: storage decl explicit binding slot =====

TEST_CASE(ir_storage_lowers_with_explicit_binding) {
    // `storage foo : rwstructuredbuffer<int> binding 2;` — the
    // binding literal is propagated onto the IRDeclaration's
    // storageBinding field. The BGFX backend reads this to emit
    // `layout(std430, binding = 2)`.
    auto ir = generateIR(R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int> binding 2
        }
    )");
    CHECK(ir.computes.size() == 1);
    CHECK(ir.computes.front()->declarations.size() == 1);
    const auto& decl = ir.computes.front()->declarations.front();
    CHECK(decl->kind == IRDeclaration::Kind::Storage);
    CHECK(decl->name == "counters");
    CHECK(decl->storageBinding == 2);
}

TEST_CASE(ir_storage_lowers_without_binding_keeps_default) {
    // Absence of `binding` → storageBinding stays at -1 (the default).
    // The BGFX backend auto-assigns slots at emit time, starting from
    // max(explicit bindings) + 1.
    auto ir = generateIR(R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int>
        }
    )");
    const auto& decl = ir.computes.front()->declarations.front();
    CHECK(decl->kind == IRDeclaration::Kind::Storage);
    CHECK(decl->storageBinding == -1);
}

TEST_CASE(ir_two_storage_decls_with_distinct_bindings) {
    // Two storage decls with explicit bindings: the IR carries
    // each literal as-is (the BGFX backend detects duplicates at
    // emit time, not at IR time — that keeps lowering simple).
    auto ir = generateIR(R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int> binding 0
            storage outputs  : rwstructuredbuffer<float> binding 1
        }
    )");
    CHECK(ir.computes.front()->declarations.size() == 2);
    CHECK(ir.computes.front()->declarations[0]->storageBinding == 0);
    CHECK(ir.computes.front()->declarations[1]->storageBinding == 1);
}

}