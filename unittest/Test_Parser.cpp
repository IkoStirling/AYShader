// ============================================================
// AYShader Parser Unit Tests (Phase 1 closure — vertex/fragment syntax)
// ============================================================

#include "AYLexer.h"
#include "AYParser.h"
#include "AYAst.h"
#include "AYTest.h"

using namespace ayt::shader::phoskia;

// Helper: tokenize + parse in one step
static std::unique_ptr<Program> parseSource(const std::string& src) {
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    return parser.parse();
}

// Helper: count declarations of a given type via dynamic_cast
template <typename T>
static int countDecls(const Program& prog) {
    int n = 0;
    for (const auto& d : prog.declarations) {
        if (dynamic_cast<const T*>(d.get())) n++;
    }
    return n;
}

TEST_SUITE(ParserTests)

// ===== Empty / minimal =====

TEST_CASE(empty_input) {
    auto prog = parseSource("");
    CHECK(prog != nullptr);
    CHECK(prog->declarations.empty());
    CHECK(prog->declarations.size() == 0);
}

TEST_CASE(only_whitespace) {
    auto prog = parseSource("   \n\t  ");
    CHECK(prog != nullptr);
    CHECK(prog->declarations.empty());
}

// ===== Material declaration =====

TEST_CASE(material_declaration_minimal) {
    auto prog = parseSource("material Unlit { vertex { } fragment { } }");
    CHECK(prog != nullptr);
    CHECK(prog->declarations.size() == 1);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    CHECK(mat->name == "Unlit");
    CHECK(mat->declarations.size() == 2);
    CHECK(dynamic_cast<VertexFunc*>(mat->declarations[0].get()) != nullptr);
    CHECK(dynamic_cast<FragmentFunc*>(mat->declarations[1].get()) != nullptr);
}

TEST_CASE(material_declaration_with_property) {
    auto prog = parseSource(
        "material X { property color = vec4(1.0, 0.0, 0.0, 1.0); vertex { } fragment { } }");
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    CHECK(mat->declarations.size() == 3);
    auto* prop = dynamic_cast<PropertyDecl*>(mat->declarations[0].get());
    CHECK(prop != nullptr);
    CHECK(prop->name == "color");
}

TEST_CASE(material_declaration_with_uniform) {
    auto prog = parseSource(
        "material X { uniform mat4 modelMatrix; vertex { } fragment { } }");
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    auto* uni = dynamic_cast<UniformDecl*>(mat->declarations[0].get());
    CHECK(uni != nullptr);
    CHECK(uni->type == "mat4");
    CHECK(uni->name == "modelMatrix");
}

TEST_CASE(material_declaration_with_texture) {
    auto prog = parseSource(
        "material X { texture2d albedoMap; vertex { } fragment { } }");
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* tex = dynamic_cast<TextureDecl*>(mat->declarations[0].get());
    CHECK(tex != nullptr);
    CHECK(tex->name == "albedoMap");
}

TEST_CASE(material_declaration_mixed) {
    const char* src = R"(
        material PBR {
            texture2d albedoMap
            uniform mat4 modelMatrix
            property color = vec3(1.0)
            property metallic = 0.5
            vertex { }
            fragment { }
        }
    )";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    CHECK(mat->declarations.size() == 6);  // tex + uniform + 2 props + vs + fs
}

TEST_CASE(multiple_materials) {
    const char* src = R"(
        material A { vertex { } fragment { } }
        material B { vertex { } fragment { } }
    )";
    auto prog = parseSource(src);
    CHECK(prog->declarations.size() == 2);
    CHECK(countDecls<MaterialDecl>(*prog) == 2);
}

// ===== Vertex block =====

TEST_CASE(vertex_with_in_params) {
    const char* src = R"(
        material X {
            vertex {
                in pos : position
                in nrm : normal
                in uv  : texcoord
                return vec4(pos, 1.0)
            }
            fragment { }
        }
    )";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* vs = dynamic_cast<VertexFunc*>(mat->declarations[0].get());
    CHECK(vs != nullptr);
    CHECK(vs->params.size() == 3);
    CHECK(vs->body.size() == 1);

    auto* p0 = dynamic_cast<ShaderParam*>(vs->params[0].get());
    CHECK(p0 != nullptr);
    CHECK(p0->dir == ShaderParam::Direction::In);
    CHECK(p0->name == "pos");
    CHECK(p0->semantic == PhoskiaSemantic::Position);

    auto* p1 = dynamic_cast<ShaderParam*>(vs->params[1].get());
    CHECK(p1->semantic == PhoskiaSemantic::Normal);

    auto* p2 = dynamic_cast<ShaderParam*>(vs->params[2].get());
    CHECK(p2->semantic == PhoskiaSemantic::Texcoord);

    auto* ret = dynamic_cast<ReturnStmt*>(vs->body[0].get());
    CHECK(ret != nullptr);
    // `return vec4(pos, 1.0)` parses as ReturnStmt carrying a CallExpr
    // (callee IdentifierExpr("vec4"), single arg IdentifierExpr("pos")).
    // The gl_Position binding is added by the BGFX converter; the AST
    // carries no gl_* identifier.
    auto* call = dynamic_cast<CallExpr*>(ret->value.get());
    CHECK(call != nullptr);
    auto* callee = dynamic_cast<IdentifierExpr*>(call->callee.get());
    CHECK(callee != nullptr);
    CHECK(callee->name == "vec4");
    CHECK(call->args.size() == 2);
    auto* arg0 = dynamic_cast<IdentifierExpr*>(call->args[0].get());
    CHECK(arg0 != nullptr);
    CHECK(arg0->name == "pos");
    auto* arg1 = dynamic_cast<LiteralExpr*>(call->args[1].get());
    CHECK(arg1 != nullptr);
}

TEST_CASE(vertex_with_out_default) {
    const char* src = R"(
        material X {
            vertex {
                in  pos : position
                out nrm : normal = vec3(0.0, 1.0, 0.0)
                return vec4(pos, 1.0)
            }
            fragment { }
        }
    )";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* vs = dynamic_cast<VertexFunc*>(mat->declarations[0].get());
    CHECK(vs != nullptr);
    CHECK(vs->params.size() == 2);
    auto* p1 = dynamic_cast<ShaderParam*>(vs->params[1].get());
    CHECK(p1 != nullptr);
    CHECK(p1->dir == ShaderParam::Direction::Out);
    CHECK(p1->semantic == PhoskiaSemantic::Normal);
    CHECK(p1->defaultValue != nullptr);
    // defaultValue is vec3(0.0, 1.0, 0.0) — a constructor call, not a
    // bare LiteralExpr. Confirm both the CallExpr shape and that the
    // callee name is "vec3".
    auto* call = dynamic_cast<CallExpr*>(p1->defaultValue.get());
    CHECK(call != nullptr);
    auto* callee = dynamic_cast<IdentifierExpr*>(call->callee.get());
    CHECK(callee != nullptr);
    CHECK(callee->name == "vec3");
}

TEST_CASE(shading_keyword_now_rejected) {
    // Phase 1 closure: 'shading' is gone, vertex/fragment replace it.
    // A 'shading' block is now an unknown token sequence that triggers
    // a parser error.
    const char* src = "material X { shading { return 1.0 } }";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    parser.parse();
    CHECK(parser.hasErrors());
}

// ===== Fragment block =====

TEST_CASE(fragment_with_in_params) {
    const char* src = R"(
        material X {
            vertex { in pos : position; return vec4(pos, 1.0) }
            fragment {
                in nrm : normal
                in clr : color
                return clr
            }
        }
    )";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* fs = dynamic_cast<FragmentFunc*>(mat->declarations[1].get());
    CHECK(fs != nullptr);
    CHECK(fs->inputs.size() == 2);
    auto* p0 = dynamic_cast<ShaderParam*>(fs->inputs[0].get());
    CHECK(p0 != nullptr);
    CHECK(p0->dir == ShaderParam::Direction::In);
    CHECK(p0->semantic == PhoskiaSemantic::Normal);
}

TEST_CASE(fragment_out_is_error) {
    // 'out' inside a fragment block is meaningless — parser records an
    // error but still consumes the token so it can recover.
    const char* src = R"(
        material X {
            vertex { return vec4(0.0) }
            fragment {
                out nrm : normal
                return vec4(0.0)
            }
        }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    parser.parse();
    CHECK(parser.hasErrors());
}

// ===== Body statements inside vertex / fragment =====

TEST_CASE(vertex_let_and_return) {
    const char* src = R"(
        material X {
            vertex {
                in pos : position
                let wpos = pos * 2.0
                return vec4(wpos, 1.0)
            }
            fragment { }
        }
    )";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* vs = dynamic_cast<VertexFunc*>(mat->declarations[0].get());
    CHECK(vs != nullptr);
    CHECK(vs->body.size() == 2);
    CHECK(dynamic_cast<LetStmt*>(vs->body[0].get()) != nullptr);
    CHECK(dynamic_cast<ReturnStmt*>(vs->body[1].get()) != nullptr);
}

TEST_CASE(fragment_if_else) {
    const char* src = R"(
        material X {
            vertex { return vec4(0.0) }
            fragment {
                in uv : texcoord
                if (uv.x > 0.5) {
                    return vec4(1.0, 0.0, 0.0, 1.0)
                } else {
                    return vec4(0.0, 0.0, 1.0, 1.0)
                }
            }
        }
    )";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* fs = dynamic_cast<FragmentFunc*>(mat->declarations[1].get());
    CHECK(fs != nullptr);
    CHECK(fs->body.size() == 1);
    auto* ifs = dynamic_cast<IfStmt*>(fs->body[0].get());
    CHECK(ifs != nullptr);
    CHECK(ifs->elseBranch.size() == 1);
}

// ===== Expressions (unchanged from Phase 1) =====

TEST_CASE(expression_binary) {
    const char* src = "material X { vertex { } fragment { in uv : texcoord; return uv * uv } }";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* fs = dynamic_cast<FragmentFunc*>(mat->declarations[1].get());
    auto* ret = dynamic_cast<ReturnStmt*>(fs->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(ret->value.get());
    CHECK(bin != nullptr);
    CHECK(bin->op.type == TokenType::Star);
}

TEST_CASE(expression_call) {
    const char* src = "material X { vertex { } fragment { in v : position; return normalize(v) } }";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* fs = dynamic_cast<FragmentFunc*>(mat->declarations[1].get());
    auto* ret = dynamic_cast<ReturnStmt*>(fs->body[0].get());
    auto* call = dynamic_cast<CallExpr*>(ret->value.get());
    CHECK(call != nullptr);
    CHECK(call->args.size() == 1);
}

TEST_CASE(expression_member) {
    const char* src = "material X { vertex { } fragment { in c : color; return c.rgb } }";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* fs = dynamic_cast<FragmentFunc*>(mat->declarations[1].get());
    auto* ret = dynamic_cast<ReturnStmt*>(fs->body[0].get());
    auto* mem = dynamic_cast<MemberExpr*>(ret->value.get());
    CHECK(mem != nullptr);
    CHECK(mem->member == "rgb");
}

TEST_CASE(expression_literal_int) {
    const char* src = "material X { vertex { } fragment { return 42 } }";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* fs = dynamic_cast<FragmentFunc*>(mat->declarations[1].get());
    auto* ret = dynamic_cast<ReturnStmt*>(fs->body[0].get());
    auto* lit = dynamic_cast<LiteralExpr*>(ret->value.get());
    CHECK(lit != nullptr);
    CHECK(std::get<int>(lit->value) == 42);
}

TEST_CASE(expression_literal_float) {
    const char* src = "material X { vertex { } fragment { return 3.14 } }";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* fs = dynamic_cast<FragmentFunc*>(mat->declarations[1].get());
    auto* ret = dynamic_cast<ReturnStmt*>(fs->body[0].get());
    auto* lit = dynamic_cast<LiteralExpr*>(ret->value.get());
    CHECK(lit != nullptr);
    CHECK(std::get<float>(lit->value) == 3.14f);
}

// ===== Variant attribute =====

TEST_CASE(variant_attribute) {
    const char* src = R"(
        material X {
            [ variant useEmission ]
            property emission = vec3(0.0)
            vertex { }
            fragment { }
        }
    )";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    // First decl is VariantAttribute, second is PropertyDecl, then vs+fs
    CHECK(mat->declarations.size() >= 4);
    auto* attr = dynamic_cast<VariantAttribute*>(mat->declarations[0].get());
    CHECK(attr != nullptr);
    CHECK(attr->name == "useEmission");
}

// ===== Error reporting =====

TEST_CASE(no_error_on_valid_source) {
    const char* src = R"(
        material X {
            property y = 1.0
            vertex { in p : position; return vec4(p, 1.0) }
            fragment { return vec4(y) }
        }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    parser.parse();
    CHECK(!parser.hasErrors());
}

TEST_CASE(missing_vertex_block_is_error) {
    const char* src = "material X { fragment { return vec4(1.0) } }";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    parser.parse();
    CHECK(parser.hasErrors());
}

TEST_CASE(missing_fragment_block_is_error) {
    const char* src = "material X { vertex { return vec4(0.0) } }";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    parser.parse();
    CHECK(parser.hasErrors());
}

TEST_SUITE_END
