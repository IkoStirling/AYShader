// ============================================================
// AYShader BGFX Backend Converter Unit Tests
// ============================================================

#include "AYPhoskia.h"
#include "AYBGFXConverter.h"
#include "AYTest.h"

using namespace ayt::shader;
using namespace ayt::shader::phoskia;

TEST_SUITE(BGFXConverterTests)

// ===== Direct AST construction for testing =====

static std::unique_ptr<Program> buildMaterial(const std::string& name) {
    auto prog = std::make_unique<Program>(std::vector<StmtPtr>{});
    auto mat = std::make_unique<MaterialDecl>(
        name,
        std::vector<StmtPtr>{});
    prog->declarations.push_back(std::move(mat));
    return prog;
}

// ===== Platform / extension =====

TEST_CASE(platform_is_bgfx) {
    AYBGFXConverter conv;
    CHECK(conv.targetPlatform() == Platform::BGFX);
    CHECK(std::string(conv.targetExtension()) == ".sc");
}

// ===== Empty / minimal material =====

TEST_CASE(empty_material_produces_block) {
    AYBGFXConverter conv;
    auto prog = buildMaterial("Unlit");
    conv.setShaderType(BGFXShaderType::Fragment);

    auto result = conv.convert(*prog);
    CHECK(result.success);
    CHECK(!result.output.empty());
    // Should contain the material name as block identifier
    CHECK(result.output.find("[Unlit]") != std::string::npos);
    CHECK(result.output.find("[fragment]") != std::string::npos);
}

TEST_CASE(vertex_shader_type) {
    AYBGFXConverter conv;
    conv.setShaderType(BGFXShaderType::Vertex);
    auto prog = buildMaterial("X");
    auto result = conv.convert(*prog);
    CHECK(result.success);
    CHECK(result.output.find("[vertex]") != std::string::npos);
}

// ===== Uniform emission =====

TEST_CASE(uniform_emitted_in_output) {
    AYBGFXConverter conv;
    auto prog = std::make_unique<Program>(std::vector<StmtPtr>{});
    auto mat = std::make_unique<MaterialDecl>("PBR", std::vector<StmtPtr>{});
    mat->declarations.push_back(
        std::make_unique<UniformDecl>("mat4", "modelMatrix"));
    mat->declarations.push_back(
        std::make_unique<UniformDecl>("vec3", "cameraPos"));
    prog->declarations.push_back(std::move(mat));

    auto result = conv.convert(*prog);
    CHECK(result.success);
    CHECK(result.output.find("uniform mat4 modelMatrix") != std::string::npos);
    CHECK(result.output.find("uniform vec3 cameraPos") != std::string::npos);
    CHECK(result.uniforms.size() == 2);
    CHECK(result.uniforms[0].name == "modelMatrix");
    CHECK(result.uniforms[0].type == "mat4");
    CHECK(result.uniforms[1].name == "cameraPos");
}

// ===== Texture emission =====

TEST_CASE(texture_emitted_with_binding) {
    AYBGFXConverter conv;
    auto prog = std::make_unique<Program>(std::vector<StmtPtr>{});
    auto mat = std::make_unique<MaterialDecl>("PBR", std::vector<StmtPtr>{});
    mat->declarations.push_back(std::make_unique<TextureDecl>("albedoMap"));
    mat->declarations.push_back(std::make_unique<TextureDecl>("normalMap"));
    prog->declarations.push_back(std::move(mat));

    auto result = conv.convert(*prog);
    CHECK(result.success);
    CHECK(result.output.find("texture2d albedoMap") != std::string::npos);
    CHECK(result.output.find("texture2d normalMap") != std::string::npos);
    CHECK(result.textures.size() == 2);
    CHECK(result.textures[0].name == "albedoMap");
    CHECK(result.textures[0].binding == 0);
    CHECK(result.textures[1].name == "normalMap");
    CHECK(result.textures[1].binding == 1);
}

// ===== Property emission =====

TEST_CASE(property_float_literal) {
    AYBGFXConverter conv;
    auto prog = std::make_unique<Program>(std::vector<StmtPtr>{});
    auto mat = std::make_unique<MaterialDecl>("X", std::vector<StmtPtr>{});
    auto prop = std::make_unique<PropertyDecl>(
        "intensity",
        std::make_unique<LiteralExpr>(0.5f));
    mat->declarations.push_back(std::move(prop));
    prog->declarations.push_back(std::move(mat));

    auto result = conv.convert(*prog);
    CHECK(result.success);
    CHECK(result.output.find("uniform vec4 intensity = 0.5") != std::string::npos);
}

TEST_CASE(property_bool_literal_emits_float) {
    AYBGFXConverter conv;
    auto prog = std::make_unique<Program>(std::vector<StmtPtr>{});
    auto mat = std::make_unique<MaterialDecl>("X", std::vector<StmtPtr>{});
    auto prop = std::make_unique<PropertyDecl>(
        "flag",
        std::make_unique<LiteralExpr>(true));
    mat->declarations.push_back(std::move(prop));
    prog->declarations.push_back(std::move(mat));

    auto result = conv.convert(*prog);
    CHECK(result.success);
    CHECK(result.output.find("1.0") != std::string::npos);
}

// ===== Shading function emission =====

TEST_CASE(shading_emits_main) {
    AYBGFXConverter conv;
    auto prog = std::make_unique<Program>(std::vector<StmtPtr>{});
    auto mat = std::make_unique<MaterialDecl>("X", std::vector<StmtPtr>{});
    auto shading = std::make_unique<ShadingFunc>(std::vector<StmtPtr>{});
    shading->body.push_back(std::make_unique<ReturnStmt>(
        std::make_unique<IdentifierExpr>("color")));
    mat->declarations.push_back(std::move(shading));
    prog->declarations.push_back(std::move(mat));

    auto result = conv.convert(*prog);
    CHECK(result.success);
    CHECK(result.output.find("void main()") != std::string::npos);
    CHECK(result.output.find("gl_FragColor = color") != std::string::npos);
}

TEST_CASE(shading_let_emission) {
    AYBGFXConverter conv;
    auto prog = std::make_unique<Program>(std::vector<StmtPtr>{});
    auto mat = std::make_unique<MaterialDecl>("X", std::vector<StmtPtr>{});
    auto shading = std::make_unique<ShadingFunc>(std::vector<StmtPtr>{});
    shading->body.push_back(std::make_unique<LetStmt>(
        "x",
        std::make_unique<LiteralExpr>(1.0f)));
    shading->body.push_back(std::make_unique<ReturnStmt>(
        std::make_unique<IdentifierExpr>("x")));
    mat->declarations.push_back(std::move(shading));
    prog->declarations.push_back(std::move(mat));

    auto result = conv.convert(*prog);
    CHECK(result.success);
    CHECK(result.output.find("let x = 1") != std::string::npos);
    CHECK(result.output.find("gl_FragColor = x") != std::string::npos);
}

// ===== Binary expression emission =====

TEST_CASE(binary_expression_in_return) {
    AYBGFXConverter conv;
    auto prog = std::make_unique<Program>(std::vector<StmtPtr>{});
    auto mat = std::make_unique<MaterialDecl>("X", std::vector<StmtPtr>{});
    auto shading = std::make_unique<ShadingFunc>(std::vector<StmtPtr>{});

    auto lhs = std::make_unique<IdentifierExpr>("a");
    auto rhs = std::make_unique<IdentifierExpr>("b");
    Token plus;
    plus.type = TokenType::Plus;
    plus.lexeme = "+";
    auto bin = std::make_unique<BinaryExpr>(std::move(lhs), plus, std::move(rhs));
    shading->body.push_back(std::make_unique<ReturnStmt>(std::move(bin)));
    mat->declarations.push_back(std::move(shading));
    prog->declarations.push_back(std::move(mat));

    auto result = conv.convert(*prog);
    CHECK(result.success);
    CHECK(result.output.find("a + b") != std::string::npos);
}

TEST_CASE(call_expression_in_return) {
    AYBGFXConverter conv;
    auto prog = std::make_unique<Program>(std::vector<StmtPtr>{});
    auto mat = std::make_unique<MaterialDecl>("X", std::vector<StmtPtr>{});
    auto shading = std::make_unique<ShadingFunc>(std::vector<StmtPtr>{});

    auto arg = std::make_unique<IdentifierExpr>("v");
    std::vector<ExprPtr> args;
    args.push_back(std::move(arg));
    auto call = std::make_unique<CallExpr>(
        std::make_unique<IdentifierExpr>("normalize"),
        std::move(args));
    shading->body.push_back(std::make_unique<ReturnStmt>(std::move(call)));
    mat->declarations.push_back(std::move(shading));
    prog->declarations.push_back(std::move(mat));

    auto result = conv.convert(*prog);
    CHECK(result.success);
    CHECK(result.output.find("normalize(v)") != std::string::npos);
}

TEST_CASE(member_expression_in_return) {
    AYBGFXConverter conv;
    auto prog = std::make_unique<Program>(std::vector<StmtPtr>{});
    auto mat = std::make_unique<MaterialDecl>("X", std::vector<StmtPtr>{});
    auto shading = std::make_unique<ShadingFunc>(std::vector<StmtPtr>{});

    auto member = std::make_unique<MemberExpr>(
        std::make_unique<IdentifierExpr>("v"), "xyz");
    shading->body.push_back(std::make_unique<ReturnStmt>(std::move(member)));
    mat->declarations.push_back(std::move(shading));
    prog->declarations.push_back(std::move(mat));

    auto result = conv.convert(*prog);
    CHECK(result.success);
    CHECK(result.output.find("v.xyz") != std::string::npos);
}

TEST_CASE(index_expression_in_return) {
    AYBGFXConverter conv;
    auto prog = std::make_unique<Program>(std::vector<StmtPtr>{});
    auto mat = std::make_unique<MaterialDecl>("X", std::vector<StmtPtr>{});
    auto shading = std::make_unique<ShadingFunc>(std::vector<StmtPtr>{});

    auto idx = std::make_unique<IndexExpr>(
        std::make_unique<IdentifierExpr>("arr"),
        std::make_unique<LiteralExpr>(0));
    shading->body.push_back(std::make_unique<ReturnStmt>(std::move(idx)));
    mat->declarations.push_back(std::move(shading));
    prog->declarations.push_back(std::move(mat));

    auto result = conv.convert(*prog);
    CHECK(result.success);
    CHECK(result.output.find("arr[0]") != std::string::npos);
}

TEST_CASE(unary_expression_in_return) {
    AYBGFXConverter conv;
    auto prog = std::make_unique<Program>(std::vector<StmtPtr>{});
    auto mat = std::make_unique<MaterialDecl>("X", std::vector<StmtPtr>{});
    auto shading = std::make_unique<ShadingFunc>(std::vector<StmtPtr>{});

    Token minus;
    minus.type = TokenType::Minus;
    minus.lexeme = "-";
    auto un = std::make_unique<UnaryExpr>(minus, std::make_unique<IdentifierExpr>("x"));
    shading->body.push_back(std::make_unique<ReturnStmt>(std::move(un)));
    mat->declarations.push_back(std::move(shading));
    prog->declarations.push_back(std::move(mat));

    auto result = conv.convert(*prog);
    CHECK(result.success);
    CHECK(result.output.find("-x") != std::string::npos);
}

// ===== Compiler args =====

TEST_CASE(compiler_args_fragment) {
    AYBGFXConverter conv;
    conv.setShaderType(BGFXShaderType::Fragment);
    auto args = conv.getCompilerArgs();
    CHECK(args.size() == 4);
    CHECK(std::string(args[0]) == "-p");
    CHECK(std::string(args[1]) == "vulkan");
    CHECK(std::string(args[2]) == "--type");
    CHECK(std::string(args[3]) == "fragment");
}

TEST_CASE(compiler_args_vertex) {
    AYBGFXConverter conv;
    conv.setShaderType(BGFXShaderType::Vertex);
    auto args = conv.getCompilerArgs();
    CHECK(args.size() == 4);
    CHECK(std::string(args[3]) == "vertex");
}

// ===== End-to-end: source -> .sc =====

TEST_CASE(end_to_end_minimal_unlit) {
    Compiler compiler;
    const char* src = R"(
        material Unlit {
            property color = vec4(1.0, 0.0, 0.0, 1.0)
            shading {
                return color
            }
        }
    )";
    auto result = compiler.compile(src);
    CHECK(result.success);
    // Output should be BGFX .sc content
    CHECK(result.output.find("[fragment]") != std::string::npos);
    CHECK(result.output.find("[Unlit]") != std::string::npos);
    CHECK(result.output.find("void main()") != std::string::npos);
    CHECK(result.output.find("color") != std::string::npos);
}

TEST_SUITE_END
