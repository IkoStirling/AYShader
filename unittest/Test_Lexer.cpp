// ============================================================
// AYShader Lexer Unit Tests
// ============================================================

#include "AYLexer.h"
#include "AYToken.h"
#include "AYTest.h"

using namespace ayt::shader::phoskia;

TEST_SUITE(LexerTests)

// ===== Keyword recognition =====

TEST_CASE(keyword_material) {
    Lexer lexer("material");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 2);  // 'material' + EOF
    CHECK(tokens[0].type == TokenType::Material);
    CHECK(tokens[0].lexeme == "material");
    CHECK(tokens[1].type == TokenType::EndOfFile);
}

TEST_CASE(keyword_property) {
    Lexer lexer("property");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 2);
    CHECK(tokens[0].type == TokenType::Property);
}

TEST_CASE(keyword_uniform) {
    Lexer lexer("uniform vec3 cameraPos");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 4);  // uniform + Identifier + Identifier + EOF
    CHECK(tokens[0].type == TokenType::Uniform);
    CHECK(tokens[1].type == TokenType::Vec3);
    CHECK(tokens[2].type == TokenType::Identifier);
    CHECK(tokens[2].lexeme == "cameraPos");
}

TEST_CASE(keyword_texture2d) {
    Lexer lexer("texture2d");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 2);
    CHECK(tokens[0].type == TokenType::Texture2D);
}

TEST_CASE(keyword_vertex_fragment) {
    Lexer lexer("vertex fragment");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 3);
    CHECK(tokens[0].type == TokenType::Vertex);
    CHECK(tokens[1].type == TokenType::Fragment);
}

TEST_CASE(keyword_let_return_if_else_for_in_out) {
    Lexer lexer("let return if else for in out");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 8);  // 7 keywords + EOF
    CHECK(tokens[0].type == TokenType::Let);
    CHECK(tokens[1].type == TokenType::Return);
    CHECK(tokens[2].type == TokenType::If);
    CHECK(tokens[3].type == TokenType::Else);
    CHECK(tokens[4].type == TokenType::For);
    CHECK(tokens[5].type == TokenType::In);
    CHECK(tokens[6].type == TokenType::Out);
}

TEST_CASE(keyword_true_false) {
    Lexer lexer("true false");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 3);
    CHECK(tokens[0].type == TokenType::True);
    CHECK(tokens[1].type == TokenType::False);
}

// ===== Type keywords =====

TEST_CASE(keyword_types) {
    Lexer lexer("float vec2 vec3 vec4 int ivec2 ivec3 ivec4 mat2 mat3 mat4 quat bool");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 14);
    CHECK(tokens[0].type == TokenType::Float);
    CHECK(tokens[1].type == TokenType::Vec2);
    CHECK(tokens[2].type == TokenType::Vec3);
    CHECK(tokens[3].type == TokenType::Vec4);
    CHECK(tokens[4].type == TokenType::Int);
    CHECK(tokens[5].type == TokenType::IVec2);
    CHECK(tokens[6].type == TokenType::IVec3);
    CHECK(tokens[7].type == TokenType::IVec4);
    CHECK(tokens[8].type == TokenType::Mat2);
    CHECK(tokens[9].type == TokenType::Mat3);
    CHECK(tokens[10].type == TokenType::Mat4);
    CHECK(tokens[11].type == TokenType::Quat);
    CHECK(tokens[12].type == TokenType::Bool);
}

// ===== Phoskia semantic keywords =====

TEST_CASE(keyword_phoskia_semantics) {
    Lexer lexer("position normal color texcoord");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 5);  // 4 + EOF
    CHECK(tokens[0].type == TokenType::Position);
    CHECK(tokens[1].type == TokenType::Normal);
    CHECK(tokens[2].type == TokenType::Color);
    CHECK(tokens[3].type == TokenType::Texcoord);
}

// ===== Operator recognition =====

TEST_CASE(operator_single_char) {
    Lexer lexer("+ - * / %");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].type == TokenType::Plus);
    CHECK(tokens[1].type == TokenType::Minus);
    CHECK(tokens[2].type == TokenType::Star);
    CHECK(tokens[3].type == TokenType::Slash);
    CHECK(tokens[4].type == TokenType::Percent);
}

TEST_CASE(operator_assignment_vs_equality) {
    Lexer lexer("= ==");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].type == TokenType::Equal);
    CHECK(tokens[1].type == TokenType::EqualEqual);
}

TEST_CASE(operator_bang) {
    Lexer lexer("! !=");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].type == TokenType::Bang);
    CHECK(tokens[1].type == TokenType::BangEqual);
}

TEST_CASE(operator_comparison) {
    Lexer lexer("< <= > >=");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].type == TokenType::Less);
    CHECK(tokens[1].type == TokenType::LessEqual);
    CHECK(tokens[2].type == TokenType::Greater);
    CHECK(tokens[3].type == TokenType::GreaterEqual);
}

TEST_CASE(operator_logical) {
    Lexer lexer("&& ||");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].type == TokenType::And);
    CHECK(tokens[1].type == TokenType::Or);
}

TEST_CASE(operator_ampersand_alone_is_unknown) {
    // '&' alone (without '&') is not a valid token
    Lexer lexer("&");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].type == TokenType::Unknown);
}

TEST_CASE(operator_pipe_alone_is_unknown) {
    Lexer lexer("|");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].type == TokenType::Unknown);
}

// ===== Punctuation =====

TEST_CASE(punctuation) {
    Lexer lexer("(){}[] , : ; .");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].type == TokenType::LeftParen);
    CHECK(tokens[1].type == TokenType::RightParen);
    CHECK(tokens[2].type == TokenType::LeftBrace);
    CHECK(tokens[3].type == TokenType::RightBrace);
    CHECK(tokens[4].type == TokenType::LeftBracket);
    CHECK(tokens[5].type == TokenType::RightBracket);
    CHECK(tokens[6].type == TokenType::Comma);
    CHECK(tokens[7].type == TokenType::Colon);
    CHECK(tokens[8].type == TokenType::Semicolon);
    CHECK(tokens[9].type == TokenType::Dot);
}

// ===== Identifier =====

TEST_CASE(identifier_basic) {
    Lexer lexer("foo bar_baz _x123");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 4);
    CHECK(tokens[0].type == TokenType::Identifier);
    CHECK(tokens[0].lexeme == "foo");
    CHECK(tokens[1].type == TokenType::Identifier);
    CHECK(tokens[1].lexeme == "bar_baz");
    CHECK(tokens[2].type == TokenType::Identifier);
    CHECK(tokens[2].lexeme == "_x123");
}

TEST_CASE(identifier_starts_with_underscore) {
    Lexer lexer("_cameraPos");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].type == TokenType::Identifier);
    CHECK(tokens[0].lexeme == "_cameraPos");
}

TEST_CASE(identifier_looks_like_keyword_prefix_is_identifier) {
    // "materialize" is not the "material" keyword
    Lexer lexer("materialize");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].type == TokenType::Identifier);
    CHECK(tokens[0].lexeme == "materialize");
}

// ===== Number literals =====

TEST_CASE(number_integer) {
    Lexer lexer("42");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 2);
    CHECK(tokens[0].type == TokenType::IntLiteral);
    CHECK(tokens[0].lexeme == "42");
}

TEST_CASE(number_float) {
    Lexer lexer("3.14");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 2);
    CHECK(tokens[0].type == TokenType::FloatLiteral);
    CHECK(tokens[0].lexeme == "3.14");
}

TEST_CASE(number_zero) {
    Lexer lexer("0");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].type == TokenType::IntLiteral);
    CHECK(tokens[0].lexeme == "0");
}

TEST_CASE(number_zero_point_not_float) {
    // "0." without following digit is treated as 0 then '.'
    Lexer lexer("0.");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].type == TokenType::IntLiteral);
    CHECK(tokens[1].type == TokenType::Dot);
}

TEST_CASE(number_mixed) {
    Lexer lexer("1 2 3.5 100.0");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 5);
    CHECK(tokens[0].lexeme == "1");
    CHECK(tokens[1].lexeme == "2");
    CHECK(tokens[2].lexeme == "3.5");
    CHECK(tokens[3].lexeme == "100.0");
}

// ===== String literals =====

TEST_CASE(string_literal) {
    Lexer lexer("\"hello\"");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 2);
    CHECK(tokens[0].type == TokenType::StringLiteral);
    CHECK(tokens[0].lexeme == "hello");
}

TEST_CASE(string_literal_empty) {
    Lexer lexer("\"\"");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 2);
    CHECK(tokens[0].type == TokenType::StringLiteral);
    CHECK(tokens[0].lexeme == "");
}

TEST_CASE(string_literal_with_spaces) {
    Lexer lexer("\"hello world\"");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].type == TokenType::StringLiteral);
    CHECK(tokens[0].lexeme == "hello world");
}

// ===== Whitespace and newlines =====

TEST_CASE(skip_whitespace) {
    Lexer lexer("  material   Unlit  ");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 3);
    CHECK(tokens[0].type == TokenType::Material);
    CHECK(tokens[1].type == TokenType::Identifier);
    CHECK(tokens[1].lexeme == "Unlit");
}

TEST_CASE(newline_increments_line) {
    Lexer lexer("a\nb");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].line == 1);
    CHECK(tokens[1].line == 2);
}

TEST_CASE(multiline_input) {
    Lexer lexer("material X {\n    property a = 1.0;\n}");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    // material, X, {, property, a, =, 1.0, ;, }, EOF = 10
    CHECK(tokens.size() == 10);
    CHECK(tokens[0].type == TokenType::Material);
    CHECK(tokens[2].type == TokenType::LeftBrace);
    CHECK(tokens[3].type == TokenType::Property);
    CHECK(tokens[7].type == TokenType::Semicolon);
    CHECK(tokens[8].type == TokenType::RightBrace);
}

// ===== Line / column tracking =====

TEST_CASE(line_column_tracking) {
    Lexer lexer("material\n  X");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].line == 1);
    CHECK(tokens[0].lexeme == "material");
    CHECK(tokens[1].line == 2);
    CHECK(tokens[1].column == 3);  // 2 spaces + start
}

// ===== Realistic snippet =====

TEST_CASE(realistic_material_declaration) {
    const char* src = R"(
        material PBR {
            texture2d albedoMap
            uniform vec3 cameraPos
            property color = vec3(1.0, 0.5, 0.25)
            vertex {
                in pos : position
                in nrm : normal
                let N = normalize(nrm)
                return vec4(pos, 1.0)
            }
            fragment { return color * N }
        }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    // Should produce many tokens and end with EOF
    CHECK(tokens.size() > 30);
    CHECK(tokens.back().type == TokenType::EndOfFile);
    // Specific checks
    bool foundMaterial = false, foundProperty = false, foundVertex = false,
         foundFragment = false, foundReturn = false;
    for (const auto& t : tokens) {
        if (t.type == TokenType::Material) foundMaterial = true;
        if (t.type == TokenType::Property) foundProperty = true;
        if (t.type == TokenType::Vertex) foundVertex = true;
        if (t.type == TokenType::Fragment) foundFragment = true;
        if (t.type == TokenType::Return) foundReturn = true;
    }
    CHECK(foundMaterial);
    CHECK(foundProperty);
    CHECK(foundVertex);
    CHECK(foundFragment);
    CHECK(foundReturn);
}

TEST_CASE(variant_attribute_syntax) {
    // Phoskia `[variant name]` syntax: 5 tokens — `[` `variant` `<name>` `]` EOF.
    Lexer lexer("[ variant useEmission ]");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 5);
    CHECK(tokens[0].type == TokenType::LeftBracket);
    CHECK(tokens[1].type == TokenType::Variant);
    CHECK(tokens[1].lexeme == "variant");
    CHECK(tokens[2].type == TokenType::Identifier);
    CHECK(tokens[2].lexeme == "useEmission");
    CHECK(tokens[3].type == TokenType::RightBracket);
}

TEST_SUITE_END
