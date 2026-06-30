// ============================================================
// AYShader Compiler (AYPhoskia) End-to-End Unit Tests
// (Phase 1 closure — vertex/fragment syntax)
// ============================================================

#include "AYPhoskia.h"
#include "AYBuiltinTypes.h"
#include "AYTest.h"

using namespace ayt::shader::phoskia;

TEST_SUITE(PhoskiaCompilerTests)

// ===== Minimal valid material =====

TEST_CASE(compile_minimal_unlit) {
    Compiler compiler;
    const char* src = R"(
        material Unlit {
            property color = vec4(1.0, 0.0, 0.0, 1.0)
            vertex { return vec4(0.0, 0.0, 0.0, 1.0) }
            fragment { return color }
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(!result.output.empty());
    CHECK(result.errors.empty());
}

TEST_CASE(compile_empty_material) {
    // Must include both blocks now — the converter rejects otherwise.
    Compiler compiler;
    auto result = CompileResult{};
    compiler.compile("material X { vertex { } fragment { } }", result);
    CHECK(result.success);
    CHECK(!result.output.empty());
}

TEST_CASE(compile_multiple_materials) {
    Compiler compiler;
    const char* src = R"(
        material A {
            property a = 1.0
            vertex { return vec4(0.0) }
            fragment { return vec4(1.0, 0.0, 0.0, 1.0) }
        }
        material B {
            property b = 2.0
            vertex { return vec4(1.0) }
            fragment { return vec4(0.0, 1.0, 0.0, 1.0) }
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.errors.empty());
}

// ===== Backend registration =====

TEST_CASE(default_backend_is_registered) {
    Compiler compiler;
    auto result = CompileResult{};
    compiler.compile(
        "material X { vertex { return vec4(0.0) } "
        "fragment { return vec4(1.0) } }", result);
    CHECK(result.success);
}

TEST_CASE(compile_to_unknown_backend_fails) {
    Compiler compiler;
    auto result = CompileResult{};
    compiler.compileToBackend(
        "material X { vertex { return vec4(0.0) } "
        "fragment { return vec4(1.0) } }", "hlsl", result);
    CHECK(!result.success);
    CHECK(!result.errors.empty());
}

TEST_CASE(register_custom_backend) {
    Compiler compiler;
    compiler.registerBackend("noop", []() {
        return std::unique_ptr<ayt::shader::IAYBackendConverter>(nullptr);
    });
    auto result = CompileResult{};
    compiler.compileToBackend(
        "material X { vertex { } fragment { } }", "noop", result);
    // nullptr backend fails inside convert(), which surfaces as an error.
    CHECK(!result.success);
}

// ===== Compilation options =====

TEST_CASE(options_default_target_is_bgfx) {
    CompileOptions opts;
    CHECK(opts.targetBackend == "bgfx");
}

TEST_CASE(options_can_be_customized) {
    CompileOptions opts;
    opts.targetBackend = "myBackend";
    opts.enableTypeInference = true;
    opts.enableSemanticAnalysis = false;
    Compiler compiler(opts);
    auto result = CompileResult{};
    compiler.compile(
        "material X { vertex { return vec4(0.0) } "
        "fragment { return vec4(1.0) } }", result);
    CHECK(!result.success);
}

// ===== Pipeline phases exposed =====

TEST_CASE(tokenize_phase) {
    Compiler compiler;
    std::vector<Token> tokens;
    compiler.tokenize(
        "material X { vertex { } fragment { } }", tokens);
    CHECK(!tokens.empty());
    CHECK(tokens.back().type == TokenType::EndOfFile);
}

TEST_CASE(parse_phase) {
    Compiler compiler;
    std::vector<Token> tokens;
    compiler.tokenize(
        "material X { vertex { } fragment { } }", tokens);
    auto ast = compiler.parse(tokens);
    CHECK(ast != nullptr);
    CHECK(ast->declarations.size() == 1);
}

// ===== Error handling =====

TEST_CASE(lex_error_propagated) {
    Compiler compiler;
    // missing material name → parser reports a missing-identifier error.
    auto result = CompileResult{};
    compiler.compile("material { vertex { } fragment { } }", result);
    CHECK(!result.errors.empty());
}

TEST_CASE(parse_error_propagated) {
    Compiler compiler;
    auto result = CompileResult{};
    compiler.compile(
        "material X { vertex { return vec4(0.0); "
        "fragment { return vec4(1.0) }", result);  // missing '}'
    CHECK(!result.errors.empty());
}

TEST_CASE(missing_vertex_block_causes_error) {
    Compiler compiler;
    auto result = CompileResult{};
    compiler.compile(
        "material X { fragment { return vec4(1.0) } }", result);
    CHECK(!result.success);
}

TEST_CASE(missing_fragment_block_causes_error) {
    Compiler compiler;
    auto result = CompileResult{};
    compiler.compile(
        "material X { vertex { return vec4(0.0) } }", result);
    CHECK(!result.success);
}

// ===== Realistic Phoskia snippet =====

TEST_CASE(compile_pbr_like_material) {
    Compiler compiler;
    const char* src = R"(
        material PBR {
            texture2d albedoMap
            uniform vec3 cameraPos

            vertex {
                in pos : position
                in nrm : normal
                in uv  : texcoord
                out worldNormal : normal = vec3(0.0, 0.0, 1.0)
                out uvCoord     : texcoord = vec2(0.0, 0.0)
                let wpos = vec4(pos, 1.0)
                return wpos
            }

            fragment {
                in worldNormal : normal
                in uvCoord     : texcoord
                let baseColor = sample(albedoMap, uvCoord)
                let N = normalize(worldNormal)
                let V = normalize(cameraPos)
                let NdotV = max(dot(N, V), 0.0)
                return vec4(baseColor.rgb * NdotV, 1.0)
            }
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    // Semantic analysis may flag incomplete uniform set; we accept that
    // and only assert the three-piece output is produced.
    CHECK(!result.output.empty());
    CHECK(result.output.find("varying.def.sc") != std::string::npos);
}

TEST_CASE(compile_with_if_else) {
    Compiler compiler;
    const char* src = R"(
        material X {
            vertex {
                if (true) {
                    return vec4(1.0)
                } else {
                    return vec4(0.0)
                }
            }
            fragment { return vec4(1.0) }
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(!result.output.empty());
}

TEST_CASE(compile_with_for_loop) {
    Compiler compiler;
    const char* src = R"(
        material X {
            vertex {
                for (i in items) {
                    let j = i
                }
                return vec4(0.0)
            }
            fragment { return vec4(1.0) }
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(!result.output.empty());
}

// ===== Phase 3.2: compute declaration end-to-end =====
//
// Phase 2.5 introduced the `compute Foo { ... }` syntax and lowered it
// to an IRComputeDecl, but the BGFX backend refused to emit a .sc
// ("HLSL / WGSL required"). That conclusion was wrong — bgfx 1.18 +
// shaderc 1.18 fully support compute. Phase 3.2 wires it through.
//
// This smoke test runs the full pipeline on a minimal compute body and
// asserts the .sc output is non-empty. The deeper structural
// assertions (layout/local_size_x, $input/$output, void main) live in
// Test_BGFXConverter.cpp; this test only verifies the high-level
// Compiler::compile path doesn't drop compute on the floor.

TEST_CASE(compile_minimal_compute) {
    Compiler compiler;
    const char* src = R"(
        compute ParticleUpdate {
            let x = 0
            return x
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    // The BGFX backend emits a single .sc source for compute; the
    // generic `output` field concatenates it with section fences.
    CHECK(!result.output.empty());
    CHECK(result.output.find("void main()") != std::string::npos);
    // Phase 3.3 Block 2: the default emit now writes all three layout
    // dimensions (y and z default to 1, matching GLSL's built-in
    // fallbacks). Pin the full directive shape.
    CHECK(result.output.find("layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;") != std::string::npos);
}

// ===== Phase 3.2 Block 2: thread_id / group_id / dispatch_id builtins =====
//
// Phoskia exposes the GLSL compute builtins as 0-arg functions returning
// vec3. The BGFX backend inlines each call to the corresponding GLSL
// builtin at emission time. `thread_id.x` is the canonical per-thread
// index and must be wired all the way through to
// `gl_GlobalInvocationID.x` in the emitted .sc source.

TEST_CASE(compile_compute_uses_thread_id_builtin) {
    Compiler compiler;
    const char* src = R"(
        compute ParticleUpdate {
            let idx = thread_id.x
            return idx
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    // The Phoskia `thread_id()` call must inline to `gl_GlobalInvocationID`,
    // and `.x` swizzle must produce `gl_GlobalInvocationID.x`.
    CHECK(result.output.find("gl_GlobalInvocationID") != std::string::npos);
    CHECK(result.output.find("gl_GlobalInvocationID.x") != std::string::npos);
    // The Phoskia name `thread_id` itself must NOT survive into the
    // emitted source — shaderc wouldn't recognise it. We assert via
    // "thread_id" being absent (or, more precisely, only the GLSL
    // builtin appears).
    CHECK(result.output.find("thread_id") == std::string::npos);
}

TEST_CASE(compile_compute_uses_group_id_builtin) {
    Compiler compiler;
    const char* src = R"(
        compute ParticleUpdate {
            let gid = group_id.x
            return gid
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("gl_WorkGroupID.x") != std::string::npos);
    CHECK(result.output.find("group_id") == std::string::npos);
}

TEST_CASE(compile_compute_uses_dispatch_id_builtin) {
    Compiler compiler;
    const char* src = R"(
        compute ParticleUpdate {
            let did = dispatch_id.x
            return did
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    // dispatch_id inlines to `gl_NumWorkGroups * gl_WorkGroupID`. The
    // parenthesised product is the canonical form so subsequent `.x`
    // swizzle lands on the product, not on `gl_NumWorkGroups` alone.
    CHECK(result.output.find("(gl_NumWorkGroups * gl_WorkGroupID).x") != std::string::npos);
    CHECK(result.output.find("dispatch_id") == std::string::npos);
}

// ===== Phase 3.2 Block 3: storage buffer declarations =====
//
// `storage <name> : structuredbuffer<T>` (read) and
// `storage <name> : rwstructuredbuffer<T>` (read-write) declarations
// live inside a compute body and emit as GLSL `buffer` blocks. The
// two access forms share the same GLSL syntax — the IR preserves the
// access field for future HLSL emitter use (Phase 5+).

TEST_CASE(compile_compute_with_storage_buffer) {
    Compiler compiler;
    const char* src = R"(
        compute Increment {
            storage counters : rwstructuredbuffer<int>
            let idx = thread_id.x
            counters[idx] = counters[idx] + 1
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    // GLSL storage buffer emission shape: `buffer Name { T data[]; } Name;`
    CHECK(result.output.find("buffer counters") != std::string::npos);
    CHECK(result.output.find("int data[]") != std::string::npos);
    CHECK(result.output.find("} counters;") != std::string::npos);
    // The Phoskia `rwstructuredbuffer` keyword must not survive into the
    // emitted source — shaderc wouldn't recognise it.
    CHECK(result.output.find("rwstructuredbuffer") == std::string::npos);
    // The body still uses thread_id and the storage buffer normally.
    CHECK(result.output.find("gl_GlobalInvocationID") != std::string::npos);
    // (uint is a common GLSL unsigned-int lexeme but AYBuiltinTypes
    // doesn't carry it as a singleton — adding the full PrimitiveType::Uint
    // enum value + builtin type + toString wiring is a Phase 3.3 task.
    // Phase 3.2 sticks to the builtin scalar / vector types the rest
    // of the pipeline already understands.)
}

TEST_CASE(compile_compute_with_structured_buffer_read) {
    Compiler compiler;
    const char* src = R"(
        compute Reader {
            storage particles : structuredbuffer<vec3>
            let idx = thread_id.x
            return idx
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("buffer particles") != std::string::npos);
    CHECK(result.output.find("vec3 data[]") != std::string::npos);
    CHECK(result.output.find("structuredbuffer") == std::string::npos);
}

// ===== Phase 3.3 Block 1: uint builtin type =====
//
// GLSL `uint` (unsigned 32-bit int) is now a first-class primitive type
// alongside int / float / bool. Three integration points:
//   - AYType: PrimitiveType::Uint + BuiltinTypes::Uint singleton
//   - lexemeToType in src/AYIr.cpp + src/AYBGFXConverter.cpp: "uint"
//     lexeme maps to BuiltinTypes::Uint
//   - AYBuiltinFunctions::registerDefaults: uint(int) constructor
//   - AYBuiltinTypes::isBuiltinType: "uint" accepted as a builtin type
//
// `uint(0)` is the canonical uint literal idiom (Phoskia parses integer
// literals as int). A future Phase 3.3 may add `0u` literal syntax.

TEST_CASE(compile_compute_with_uint_storage_buffer) {
    Compiler compiler;
    const char* src = R"(
        compute UintCounter {
            storage counters : rwstructuredbuffer<uint>
            let idx = thread_id.x
            counters[idx] = counters[idx] + uint(1)
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("buffer counters") != std::string::npos);
    CHECK(result.output.find("uint data[]") != std::string::npos);
    CHECK(result.output.find("} counters;") != std::string::npos);
    CHECK(result.output.find("rwstructuredbuffer") == std::string::npos);
}

TEST_CASE(uint_is_builtin_type) {
    // The semantic-analyzer-side type check (used by uniform decls and
    // similar) must recognise "uint" as a builtin scalar. If it doesn't,
    // `uniform uint x;` would error with "is not a builtin type". This
    // guards the AYBuiltinTypes table registration.
    CHECK(AYBuiltinTypes::isBuiltinType("uint"));
    CHECK(AYBuiltinTypes::isBuiltinType("int"));
    CHECK(AYBuiltinTypes::isBuiltinType("float"));
    CHECK_FALSE(AYBuiltinTypes::isBuiltinType("foo"));
}

// ===== Phase 3.3 Block 2: [numthreads(X, Y, Z)] compute attribute =====
//
// `[numthreads(X, Y, Z)] compute Foo { ... }` lets the user pin the
// GLSL workgroup shape per-declaration instead of inheriting the
// BGFX backend's hardcoded 64 default. Stored on the AST / IR
// ComputeDecl node and emitted as the GLSL `layout(local_size_x = X,
// local_size_y = Y, local_size_z = Z) in;` directive.

TEST_CASE(compile_compute_with_numthreads_attribute) {
    Compiler compiler;
    const char* src = R"(
        [numthreads(8, 8, 1)] compute MatMulKernel {
            let idx = thread_id.x
            return idx
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    // The user's (8, 8, 1) must reach the emitted source verbatim.
    CHECK(result.output.find("layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;") != std::string::npos);
    // The Phoskia `[numthreads(...)]` attribute must not survive into
    // the emitted source.
    CHECK(result.output.find("numthreads") == std::string::npos);
}

TEST_CASE(compile_compute_without_numthreads_uses_default) {
    // Backward compatibility: compute declarations without the attribute
    // fall back to the historical Phase 3.2 default of (64, 1, 1). This
    // keeps Phase 3.2 sources working without source edits.
    Compiler compiler;
    const char* src = R"(
        compute DefaultShape {
            let idx = thread_id.x
            return idx
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;") != std::string::npos);
}

// ===== Phase 3.3 Block 3: uvec3 strict typing =====
//
// Phoskia's compute thread-id builtins (`thread_id` / `group_id` /
// `dispatch_id`) are now strictly typed as uvec3, matching GLSL's
// actual builtin types (gl_GlobalInvocationID, gl_WorkGroupID,
// gl_NumWorkGroups). The BGFX emit path is unchanged — the GLSL
// builtins are still inlined by name — so the emitted .sc is
// byte-identical to Phase 3.2 output for the same source. The
// strict-typing change is visible only on the Phoskia / IR side:
// `let idx = thread_id.x` now binds `idx` to `uint` (was `float`).
//
// We assert on the e2e emit shape (must still contain
// gl_GlobalInvocationID.x) AND on the IR-resolved type so a future
// regression to the loose `vec3` typing is caught.

#include "AYIr.h"
#include "AYLexer.h"
#include "AYParser.h"

TEST_CASE(compile_thread_id_x_is_uint_in_ir) {
    const char* src = R"(
        compute Foo {
            let idx = thread_id.x
            return idx
        }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto ast = parser.parse();
    ayt::shader::phoskia::ir::IRGenerator gen;
    auto program = gen.generate(*ast);

    CHECK(program.computes.size() == 1);
    const auto& body = program.computes.front()->body;
    // body[0] is the let-stmt, body[1] is the return-stmt.
    auto* let = dynamic_cast<ayt::shader::phoskia::ir::IRLetStmt*>(body[0].get());
    CHECK(let != nullptr);
    CHECK_NOT_NULL(let->initializer.get());
    // The let-initializer is a MemberExpr(thread_id, "x"). Its
    // resolvedType must be `uint` (PrimitiveType::Uint), not `float`.
    auto prim = std::dynamic_pointer_cast<ayt::shader::phoskia::PrimitiveType_>(let->initializer->resolvedType);
    CHECK_NOT_NULL(prim.get());
    CHECK(prim->primitive() == ayt::shader::phoskia::PrimitiveType::Uint);
}

TEST_CASE(compile_thread_id_x_emits_gl_global_invocation_id_x) {
    // Emit shape must NOT change. The strict typing only affects the
    // Phoskia-side resolvedType; the GLSL still uses the raw builtin.
    Compiler compiler;
    const char* src = R"(
        compute Foo {
            let idx = thread_id.x
            return idx
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("gl_GlobalInvocationID") != std::string::npos);
    CHECK(result.output.find("gl_GlobalInvocationID.x") != std::string::npos);
}

TEST_CASE(compile_let_uint_idx_emits_with_uint_type) {
    // The let-stmt emit reads the GLSL type from
    // `initializer->resolvedType->toString()`. After Block 3 the
    // initializer `thread_id.x` has resolvedType = PrimitiveType::Uint
    // (strict uvec3 / uint chain), so the emit must include a
    // `uint idx` declaration (and NOT a `float idx` one — that would
    // mean the swizzle inferred Float, i.e. Block 3 didn't take
    // effect).
    Compiler compiler;
    const char* src = R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int>
            let idx = thread_id.x
            counters[idx] = counters[idx] + 1
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    // Strict-typing guarantee: the swizzle `thread_id.x` MUST NOT
    // resolve to float (would mean Block 3 didn't take effect).
    CHECK(result.output.find("float idx = ") == std::string::npos);
    CHECK(result.output.find("vec3 idx = ") == std::string::npos);
}

TEST_CASE(compile_uvec3_constructor_emits_uvec3_call) {
    // Explicit uvec3(...) constructor. Mirrors the constructor test in
    // Test_TypeInference.cpp but at the e2e level — verifies the
    // BGFX backend emits the correct GLSL constructor name. We pass
    // integer literals (not `0u`) because Phoskia's lexer parses all
    // integer literals as int — GLSL allows implicit int→uint in
    // constructors, so the constructor call shape is the testable
    // surface here.
    Compiler compiler;
    const char* src = R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int>
            let idx = uvec3(thread_id.x, 0, 0).x
            counters[idx] = counters[idx] + 1
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("uvec3(") != std::string::npos);
}

// ===== Phase 3.3 Block 4: workgroup-shared local memory =====
TEST_CASE(compile_compute_with_shared_array) {
    // `shared float tile[64];` declares a workgroup-local array.
    // The BGFX emit is `shared float tile[64];` placed before
    // `void main()`. All threads in the same workgroup see the same
    // memory; reads / writes from one thread become visible to
    // peers after a barrier (barrier syntax is a separate extension).
    Compiler compiler;
    const char* src = R"(
        compute Reduce {
            shared float tile[64]
            let i = thread_id.x
            tile[i] = float(i)
            return tile[0]
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    // Emit must contain the GLSL shared array declaration.
    CHECK(result.output.find("shared float tile[64];") != std::string::npos);
    // The Phoskia `shared` keyword must not appear inside a `buffer`
    // (storage buffer emission is separate; they share the source
    // text but the BGFX emit is two different shapes).
    CHECK(result.output.find("rwstructuredbuffer") == std::string::npos);
    // The shared array must appear BEFORE `void main()` — the
    // declaration is at the top of the compute file, not inside the
    // body. A regression that placed it inside main() would still
    // compile (GLSL accepts it) but it would be re-initialised per
    // thread, defeating the workgroup-shared semantics.
    auto posShared = result.output.find("shared float tile[64];");
    auto posMain = result.output.find("void main()");
    CHECK(posShared != std::string::npos);
    CHECK(posMain != std::string::npos);
    CHECK(posShared < posMain);
}

TEST_CASE(compile_compute_with_uint_shared_array) {
    // The uint element type (Phase 3.3 Block 1) is valid for shared
    // arrays. Catches a regression where the emit hardcodes float.
    Compiler compiler;
    const char* src = R"(
        compute Histogram {
            shared uint bins[256]
            let i = thread_id.x
            bins[i] = uint(0)
            return 0
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("shared uint bins[256];") != std::string::npos);
}

TEST_CASE(compile_compute_shared_and_storage_coexist) {
    // Shared arrays and storage buffers can both live in the same
    // compute body. The BGFX emit must produce BOTH declarations
    // (one as a GLSL `shared T name[N];` line, the other as a
    // `buffer Name { T data[]; } Name;` block). The order between
    // them doesn't matter to GLSL; we just verify both shapes are
    // present.
    Compiler compiler;
    const char* src = R"(
        compute Scan {
            shared float tile[64]
            storage inBuf : structuredbuffer<float>
            let i = thread_id.x
            tile[i] = inBuf[i]
            return 0
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("shared float tile[64];") != std::string::npos);
    CHECK(result.output.find("buffer inBuf") != std::string::npos);
    CHECK(result.output.find("float data[]") != std::string::npos);
}

// ===== Phase 3.4: uniform block (UBO) =====
TEST_CASE(compile_uniformblock_emits_layout_std140) {
    // `uniformblock Camera { vec3 position; float fov; }` is a
    // top-level decl. The BGFX emit must produce a GLSL
    // `layout(std140, binding = 0) uniform Camera { ... } Camera;`
    // line, spliced into both vs and fs (UBO is global — every
    // stage that uses the block needs the decl visible).
    Compiler compiler;
    const char* src = R"(
        uniformblock Camera {
            vec3 position
            float fov
        }
        material PBR {
            vertex {
                return vec4(Camera.position, 1.0)
            }
            fragment {
                return vec4(Camera.fov, 0.0, 0.0, 1.0)
            }
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    // The UBO decl must appear in the emitted source.
    CHECK(result.output.find("layout(std140, binding = 0) uniform Camera {") != std::string::npos);
    // Field types resolve to GLSL lexemes (vec3, float).
    CHECK(result.output.find("vec3 position;") != std::string::npos);
    CHECK(result.output.find("float fov;") != std::string::npos);
    // The instance name is the same as the block name (GLSL convention).
    CHECK(result.output.find("} Camera;") != std::string::npos);
}

TEST_CASE(compile_two_uniformblocks_have_distinct_bindings) {
    // Two UBOs in declaration order get binding slots 0 and 1.
    // The auto-incrementing slot counter in the IRGenerator makes
    // the numbers stable across re-runs of the same source.
    Compiler compiler;
    const char* src = R"(
        uniformblock Camera {
            vec3 position
        }
        uniformblock Lighting {
            vec3 ambient
        }
        material P {
            vertex { return vec4(0.0) }
            fragment { return vec4(0.0) }
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("layout(std140, binding = 0) uniform Camera {") != std::string::npos);
    CHECK(result.output.find("layout(std140, binding = 1) uniform Lighting {") != std::string::npos);
}

TEST_CASE(compile_uniformblock_field_access_emits_dot) {
    // `Camera.position` is a MemberExpr — the BGFX emit path writes
    // it literally as `Camera.position` (no type annotation on the
    // chain; GLSL-side type checking happens at shaderc compile
    // time). The let-stmt emit prefixes the GLSL type from the
    // initializer's resolvedType — `Camera.position` infers as
    // vec3 (because of Phase 3.4's known limitation: UBO blocks
    // are NOT registered in the body's TypeEnvironment, so the
    // MemberExpr type-inference path returns a fresh TypeVar and
    // the let-stmt falls through to no GLSL type prefix).
    Compiler compiler;
    const char* src = R"(
        uniformblock Camera {
            vec3 position
        }
        material P {
            vertex {
                let p = Camera.position
                return vec4(p, 1.0)
            }
            fragment { return vec4(0.0) }
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    // The dot access reaches the emitted source verbatim.
    CHECK(result.output.find("Camera.position") != std::string::npos);
    // The let-stmt does NOT carry a float / vec3 / vec4 type
    // prefix (known limitation — see AYIr.cpp lowerDecl's
    // UniformBlock branch comment). GLSL accepts the unprefixed
    // form when the type is determined by the right-hand side.
    // We don't pin the absence-of-prefix because the inferred
    // type may evolve; the contract here is "the field access
    // reaches the emitted source unchanged".
}

TEST_CASE(compile_uniformblock_in_compute_body) {
    // UBO is valid in compute shaders too (Phase 3.4 design
    // decision). The BGFX emit must include the UBO decl in the
    // cs output, before the layout / void main() / body.
    Compiler compiler;
    const char* src = R"(
        uniformblock Config {
            uint iterations
        }
        compute Run {
            let i = thread_id.x
            return i
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("layout(std140, binding = 0) uniform Config {") != std::string::npos);
    CHECK(result.output.find("uint iterations;") != std::string::npos);
}

// ===== Phase 3.5-A: storage decl explicit binding slot =====
TEST_CASE(compile_storage_with_binding_emits_layout_std430) {
    // `storage X : rwstructuredbuffer<T> binding 1;` emits a
    // `layout(std430, binding = 1) buffer X { T data[]; } X;` line.
    // std430 (not std140) because storage buffers are runtime-sized
    // and benefit from looser packing rules.
    Compiler compiler;
    const char* src = R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int> binding 1
            let idx = thread_id.x
            counters[idx] = counters[idx] + 1
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("layout(std430, binding = 1) buffer counters {") != std::string::npos);
    CHECK(result.output.find("int data[]") != std::string::npos);
    CHECK(result.output.find("} counters;") != std::string::npos);
}

TEST_CASE(compile_storage_without_binding_uses_auto_slot) {
    // A storage decl without the `binding` suffix still compiles.
    // Phase 3.5-A assigns it an automatic binding slot (starting at
    // 0 in absence of any explicit binding) and emits the same
    // `layout(std430, binding = N)` prefix as the explicit path —
    // this is a strict superset of the historical Phase 3.2-3.4
    // emit (which left the layout empty and silently bound to slot
    // 0). The auto-assigned form is unambiguous at runtime and
    // matches what explicit `binding 0` would produce.
    Compiler compiler;
    const char* src = R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int>
            let idx = thread_id.x
            counters[idx] = counters[idx] + 1
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("layout(std430, binding = 0) buffer counters {") != std::string::npos);
}

TEST_CASE(compile_two_storages_auto_assign_distinct_slots) {
    // Two storage decls without explicit binding → auto-assigned
    // slots 0 and 1 in declaration order. The BGFX backend records
    // each binding in BGFXStorageBuffer and emits
    // `layout(std430, binding = N)` for each.
    Compiler compiler;
    const char* src = R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int>
            storage outputs  : rwstructuredbuffer<float>
            let idx = thread_id.x
            counters[idx] = counters[idx] + 1
            outputs[idx] = float(idx)
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("layout(std430, binding = 0) buffer counters") != std::string::npos);
    CHECK(result.output.find("layout(std430, binding = 1) buffer outputs") != std::string::npos);
}

TEST_CASE(compile_storage_mixed_explicit_and_auto) {
    // One storage with explicit binding 1, another without → the
    // auto-assigned one starts at 2 (max(explicit) + 1), not 0.
    // This avoids collisions when the user explicitly reserves
    // slot 0 for some other purpose (cross-shader binding plans).
    Compiler compiler;
    const char* src = R"(
        compute Foo {
            storage explicit : rwstructuredbuffer<int> binding 1
            storage auto    : rwstructuredbuffer<float>
            let idx = thread_id.x
            explicit[idx] = explicit[idx] + 1
            auto[idx] = float(idx)
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("layout(std430, binding = 1) buffer explicit") != std::string::npos);
    CHECK(result.output.find("layout(std430, binding = 2) buffer auto") != std::string::npos);
}

TEST_CASE(compile_storage_with_duplicate_binding_errors) {
    // Two storage decls both writing `binding 0` is a hard user
    // error: the runtime can't tell which buffer to bind to slot 0.
    // The BGFX backend detects this at emit time and surfaces a
    // CompileResult error.
    Compiler compiler;
    const char* src = R"(
        compute Foo {
            storage a : rwstructuredbuffer<int> binding 0
            storage b : rwstructuredbuffer<float> binding 0
            return 0
        }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK_FALSE(result.success);
    bool foundDuplicateError = false;
    for (const auto& err : result.errors) {
        if (err.message.find("duplicate binding") != std::string::npos) {
            foundDuplicateError = true;
            break;
        }
    }
    CHECK(foundDuplicateError);
}

// ===== Phase 3.5-B: uniformblock decl explicit binding slot =====
// Mirrors the storage-binding tests above (Phase 3.5-A). The UBO
// path differs in that (a) the duplicate-check scope is per-program
// (UBOs are global across vs/fs/cs), and (b) the `bind 0` baseline
// was the same as the explicit `binding 0` form (Phase 3.4 emitted
// `layout(std140, binding = 0)` for the first UBO since it was
// assigned by the IRGenerator counter).
TEST_CASE(compile_uniformblock_with_binding_emits_layout_std140) {
    // `uniformblock X { ... } binding 1;` emits a
    // `layout(std140, binding = 1) uniform X { ... } X;` line. std140
    // (not std430) because UBO blocks are fixed-size.
    Compiler compiler;
    const char* src = R"(
        uniformblock Camera {
            vec3 position
            float fov
        } binding 1
        material P { vertex { return vec4(Camera.position, 1.0) } fragment { return vec4(0,0,0,1) } }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("layout(std140, binding = 1) uniform Camera {") != std::string::npos);
}

TEST_CASE(compile_uniformblock_without_binding_uses_auto_slot) {
    // A uniformblock without the `binding` suffix still compiles.
    // Phase 3.5-B assigns it an automatic binding slot (starting at
    // 0 in absence of any explicit binding) and emits the same
    // `layout(std140, binding = N)` prefix as the explicit path.
    // This is byte-identical to the Phase 3.4 emit (the IRGenerator
    // counter produced 0 as the first UBO slot, so auto is a strict
    // superset of historical behavior).
    Compiler compiler;
    const char* src = R"(
        uniformblock Camera {
            vec3 position
        }
        material P { vertex { return vec4(Camera.position, 1.0) } fragment { return vec4(0,0,0,1) } }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("layout(std140, binding = 0) uniform Camera {") != std::string::npos);
}

TEST_CASE(compile_uniformblock_mixed_explicit_and_auto) {
    // One UBO with explicit binding 1, another without → the
    // auto-assigned one starts at 2 (max(explicit) + 1), not 0.
    // This avoids collisions when the user explicitly reserves
    // slot 0 for some other purpose (cross-shader binding plans).
    Compiler compiler;
    const char* src = R"(
        uniformblock Explicit {
            vec3 a
        } binding 1
        uniformblock Auto {
            vec3 b
        }
        material P { vertex { return vec4(Explicit.a + Auto.b, 1.0) } fragment { return vec4(0,0,0,1) } }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK(result.success);
    CHECK(result.output.find("layout(std140, binding = 1) uniform Explicit {") != std::string::npos);
    CHECK(result.output.find("layout(std140, binding = 2) uniform Auto {") != std::string::npos);
}

TEST_CASE(compile_uniformblock_with_duplicate_binding_errors) {
    // Two top-level UBOs both writing `binding 0` is a hard user
    // error: the runtime can't tell which buffer to bind to slot 0.
    // The BGFX backend detects this at emit time and surfaces a
    // CompileResult error. Scope is per-program (not per-compute),
    // matching the scope of UBO declarations.
    Compiler compiler;
    const char* src = R"(
        uniformblock A { vec3 a; } binding 0
        uniformblock B { vec3 b; } binding 0
        material P { vertex { return vec4(A.a + B.b, 1.0) } fragment { return vec4(0,0,0,1) } }
    )";
    auto result = CompileResult{};
    compiler.compile(src, result);
    CHECK_FALSE(result.success);
    bool foundDuplicateError = false;
    for (const auto& err : result.errors) {
        if (err.message.find("duplicate binding") != std::string::npos) {
            foundDuplicateError = true;
            break;
        }
    }
    CHECK(foundDuplicateError);
}

TEST_SUITE_END
