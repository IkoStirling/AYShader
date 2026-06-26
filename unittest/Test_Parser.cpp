// ============================================================
// AYShader Parser Unit Tests
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
    auto prog = parseSource("material Unlit { }");
    CHECK(prog != nullptr);
    CHECK(prog->declarations.size() == 1);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    CHECK(mat->name == "Unlit");
    CHECK(mat->declarations.empty());
}

TEST_CASE(material_declaration_with_property) {
    auto prog = parseSource("material X { property color = vec3(1.0, 0.0, 0.0); }");
    CHECK(prog != nullptr);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    CHECK(mat->declarations.size() == 1);
    auto* prop = dynamic_cast<PropertyDecl*>(mat->declarations[0].get());
    CHECK(prop != nullptr);
    CHECK(prop->name == "color");
}

TEST_CASE(material_declaration_with_uniform) {
    auto prog = parseSource("material X { uniform mat4 modelMatrix; }");
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    CHECK(mat->declarations.size() == 1);
    auto* uni = dynamic_cast<UniformDecl*>(mat->declarations[0].get());
    CHECK(uni != nullptr);
    CHECK(uni->type == "mat4");
    CHECK(uni->name == "modelMatrix");
}

TEST_CASE(material_declaration_with_texture) {
    auto prog = parseSource("material X { texture2d albedoMap; }");
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
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
        }
    )";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    CHECK(mat->declarations.size() == 4);
}

TEST_CASE(multiple_materials) {
    const char* src = R"(
        material A { }
        material B { }
    )";
    auto prog = parseSource(src);
    CHECK(prog->declarations.size() == 2);
    CHECK(countDecls<MaterialDecl>(*prog) == 2);
}

// ===== Shading function =====

TEST_CASE(shading_with_let_and_return) {
    const char* src = R"(
        material X {
            shading {
                let a = 1.0
                let b = 2.0
                return a + b
            }
        }
    )";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    CHECK(mat->declarations.size() == 1);
    auto* shading = dynamic_cast<ShadingFunc*>(mat->declarations[0].get());
    CHECK(shading != nullptr);
    CHECK(shading->body.size() == 3);

    auto* let1 = dynamic_cast<LetStmt*>(shading->body[0].get());
    CHECK(let1 != nullptr);
    CHECK(let1->name == "a");

    auto* ret = dynamic_cast<ReturnStmt*>(shading->body[2].get());
    CHECK(ret != nullptr);
    CHECK(ret->value != nullptr);
}

TEST_CASE(shading_with_return_no_value) {
    const char* src = "material X { shading { return; } }";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* shading = dynamic_cast<ShadingFunc*>(mat->declarations[0].get());
    auto* ret = dynamic_cast<ReturnStmt*>(shading->body[0].get());
    CHECK(ret != nullptr);
    CHECK(ret->value == nullptr);
}

// ===== If statement =====

TEST_CASE(if_statement) {
    const char* src = R"(
        material X {
            shading {
                if (x > 0.0) {
                    return 1.0
                }
            }
        }
    )";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* shading = dynamic_cast<ShadingFunc*>(mat->declarations[0].get());
    auto* ifs = dynamic_cast<IfStmt*>(shading->body[0].get());
    CHECK(ifs != nullptr);
    CHECK(ifs->condition != nullptr);
    CHECK(ifs->thenBranch.size() == 1);
    CHECK(ifs->elseBranch.empty());
}

TEST_CASE(if_else_statement) {
    const char* src = R"(
        material X {
            shading {
                if (cond) {
                    return 1.0
                } else {
                    return 0.0
                }
            }
        }
    )";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* shading = dynamic_cast<ShadingFunc*>(mat->declarations[0].get());
    auto* ifs = dynamic_cast<IfStmt*>(shading->body[0].get());
    CHECK(ifs != nullptr);
    CHECK(ifs->thenBranch.size() == 1);
    CHECK(ifs->elseBranch.size() == 1);
}

// ===== For statement =====

TEST_CASE(for_loop) {
    const char* src = R"(
        material X {
            shading {
                for (item in list) {
                    let x = item
                }
            }
        }
    )";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* shading = dynamic_cast<ShadingFunc*>(mat->declarations[0].get());
    auto* fors = dynamic_cast<ForStmt*>(shading->body[0].get());
    CHECK(fors != nullptr);
    CHECK(fors->variable == "item");
    CHECK(fors->iterable != nullptr);
    CHECK(fors->body.size() == 1);
}

// ===== Expressions =====

TEST_CASE(expression_binary) {
    const char* src = "material X { shading { return a + b * c; } }";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* shading = dynamic_cast<ShadingFunc*>(mat->declarations[0].get());
    auto* ret = dynamic_cast<ReturnStmt*>(shading->body[0].get());
    CHECK(ret != nullptr);
    auto* outer = dynamic_cast<BinaryExpr*>(ret->value.get());
    CHECK(outer != nullptr);
    CHECK(outer->op.type == TokenType::Plus);
    // right side should be BinaryExpr (b * c) due to precedence
    auto* inner = dynamic_cast<BinaryExpr*>(outer->right.get());
    CHECK(inner != nullptr);
    CHECK(inner->op.type == TokenType::Star);
}

TEST_CASE(expression_call) {
    const char* src = "material X { shading { return normalize(v); } }";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* shading = dynamic_cast<ShadingFunc*>(mat->declarations[0].get());
    auto* ret = dynamic_cast<ReturnStmt*>(shading->body[0].get());
    auto* call = dynamic_cast<CallExpr*>(ret->value.get());
    CHECK(call != nullptr);
    CHECK(call->args.size() == 1);
    auto* callee = dynamic_cast<IdentifierExpr*>(call->callee.get());
    CHECK(callee != nullptr);
    CHECK(callee->name == "normalize");
}

TEST_CASE(expression_member) {
    const char* src = "material X { shading { return v.rgb; } }";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* shading = dynamic_cast<ShadingFunc*>(mat->declarations[0].get());
    auto* ret = dynamic_cast<ReturnStmt*>(shading->body[0].get());
    auto* member = dynamic_cast<MemberExpr*>(ret->value.get());
    CHECK(member != nullptr);
    CHECK(member->member == "rgb");
}

TEST_CASE(expression_index) {
    const char* src = "material X { shading { return arr[0]; } }";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* shading = dynamic_cast<ShadingFunc*>(mat->declarations[0].get());
    auto* ret = dynamic_cast<ReturnStmt*>(shading->body[0].get());
    auto* idx = dynamic_cast<IndexExpr*>(ret->value.get());
    CHECK(idx != nullptr);
    CHECK(idx->object != nullptr);
    CHECK(idx->index != nullptr);
}

TEST_CASE(expression_unary_minus) {
    const char* src = "material X { shading { return -x; } }";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* shading = dynamic_cast<ShadingFunc*>(mat->declarations[0].get());
    auto* ret = dynamic_cast<ReturnStmt*>(shading->body[0].get());
    auto* un = dynamic_cast<UnaryExpr*>(ret->value.get());
    CHECK(un != nullptr);
    CHECK(un->op.type == TokenType::Minus);
}

TEST_CASE(expression_literal_int) {
    const char* src = "material X { shading { return 42; } }";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* shading = dynamic_cast<ShadingFunc*>(mat->declarations[0].get());
    auto* ret = dynamic_cast<ReturnStmt*>(shading->body[0].get());
    auto* lit = dynamic_cast<LiteralExpr*>(ret->value.get());
    CHECK(lit != nullptr);
    CHECK(std::get<int>(lit->value) == 42);
}

TEST_CASE(expression_literal_float) {
    const char* src = "material X { shading { return 3.14; } }";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* shading = dynamic_cast<ShadingFunc*>(mat->declarations[0].get());
    auto* ret = dynamic_cast<ReturnStmt*>(shading->body[0].get());
    auto* lit = dynamic_cast<LiteralExpr*>(ret->value.get());
    CHECK(lit != nullptr);
    CHECK(std::get<float>(lit->value) == 3.14f);
}

TEST_CASE(expression_literal_bool) {
    const char* src = "material X { shading { return true; } }";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* shading = dynamic_cast<ShadingFunc*>(mat->declarations[0].get());
    auto* ret = dynamic_cast<ReturnStmt*>(shading->body[0].get());
    auto* lit = dynamic_cast<LiteralExpr*>(ret->value.get());
    CHECK(lit != nullptr);
    CHECK(std::get<bool>(lit->value) == true);
}

// ===== Variant attribute =====

TEST_CASE(variant_attribute) {
    const char* src = R"(
        material X {
            [ variant useEmission ]
            property emission = vec3(0.0)
        }
    )";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    // First decl is VariantAttribute, second is PropertyDecl
    CHECK(mat->declarations.size() == 2);
    auto* attr = dynamic_cast<VariantAttribute*>(mat->declarations[0].get());
    CHECK(attr != nullptr);
    CHECK(attr->name == "useEmission");
    auto* prop = dynamic_cast<PropertyDecl*>(mat->declarations[1].get());
    CHECK(prop != nullptr);
}

// ===== Error reporting =====

TEST_CASE(error_reported_on_missing_semicolon) {
    // Phase 1 decision: semicolons are optional (Python-like).
    // A missing ';' no longer produces a parser error.
    Lexer lexer("material X { property y = 1.0 }");  // no ;
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    parser.parse();
    CHECK(!parser.hasErrors());  // semicolon is optional
}

TEST_CASE(no_error_on_valid_source) {
    Lexer lexer("material X { property y = 1.0; }");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    parser.parse();
    CHECK(!parser.hasErrors());
}

TEST_SUITE_END
