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

TEST_SUITE_END
