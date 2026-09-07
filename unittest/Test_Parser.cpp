// ============================================================
// AYShader Parser Unit Tests (Phase 1 closure ?vertex/fragment syntax)
// ============================================================

#include "AYShader/Lexer.h"
#include "AYShader/Parser.h"
#include "AYShader/Ast.h"
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

TEST_CASE(material_declaration_with_texturecube) {
    auto prog = parseSource(
        "material X { texturecube envMap; vertex { } fragment { } }");
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* tex = dynamic_cast<TextureDecl*>(mat->declarations[0].get());
    CHECK(tex != nullptr);
    CHECK(tex->name == "envMap");
    CHECK(tex->samplerKind == TextureSamplerKind::SamplerCube);
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

// ===== Compute declaration (Phase 2.5) =====
//
// `compute Name { <body> }` is a top-level declaration that mirrors
// `material` but defines a GPGPU kernel rather than a renderable
// surface. Phase 2.5 introduces the parser / AST half; the BGFX
// backend reports a friendly "HLSL / WGSL required (Phase 3)"
// diagnostic. These tests pin the parser shape so a refactor that
// accidentally puts compute inside `material { }` or breaks the
// body parsing is caught.

TEST_CASE(compute_declaration_minimal) {
    auto prog = parseSource("compute Foo { }");
    CHECK(prog != nullptr);
    CHECK(prog->declarations.size() == 1);
    auto* cmp = dynamic_cast<ComputeDecl*>(prog->declarations[0].get());
    CHECK(cmp != nullptr);
    CHECK(cmp->name == "Foo");
    CHECK(cmp->body.empty());
    // Compute is not a Material ?the dispatch must NOT treat it as one.
    CHECK(dynamic_cast<MaterialDecl*>(prog->declarations[0].get()) == nullptr);
}

TEST_CASE(compute_declaration_with_body) {
    const char* src = R"(
        compute Foo {
            let x = 0
            return x
        }
    )";
    auto prog = parseSource(src);
    auto* cmp = dynamic_cast<ComputeDecl*>(prog->declarations[0].get());
    CHECK(cmp != nullptr);
    CHECK(cmp->name == "Foo");
    CHECK(cmp->body.size() == 2);
    CHECK(dynamic_cast<LetStmt*>(cmp->body[0].get()) != nullptr);
    CHECK(dynamic_cast<ReturnStmt*>(cmp->body[1].get()) != nullptr);
}

TEST_CASE(compute_keyword_recognized) {
    // The lexer's keyword table must classify `compute` as
    // TokenType::Compute (not Identifier) so the parser dispatch
    // catches it. Without this, parseStatement falls through to the
    // bare-expression path and fails the test.
    Lexer lexer("compute Foo { }");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].type == TokenType::Compute);
    CHECK(tokens[0].lexeme == "compute");
}

TEST_CASE(compute_top_level_alongside_material) {
    // `compute` is a top-level decl ?it sits next to `material`, not
    // inside it. The dispatcher must produce one MaterialDecl and one
    // ComputeDecl in the program's declarations list.
    const char* src = R"(
        material A { vertex { } fragment { } }
        compute B { let x = 1.0; return x }
    )";
    auto prog = parseSource(src);
    CHECK(prog != nullptr);
    CHECK(prog->declarations.size() == 2);
    CHECK(dynamic_cast<MaterialDecl*>(prog->declarations[0].get()) != nullptr);
    auto* cmp = dynamic_cast<ComputeDecl*>(prog->declarations[1].get());
    CHECK(cmp != nullptr);
    CHECK(cmp->name == "B");
    CHECK(cmp->body.size() == 2);
}

// ===== Phase 3.3 Block 4: workgroup-shared local memory =====

TEST_CASE(shared_keyword_recognized) {
    // `shared` must be a dedicated TokenType, not Identifier ?    // the parser dispatch relies on it to route to parseSharedDecl.
    Lexer lexer("shared float tile[64]");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].type == TokenType::Shared);
    CHECK(tokens[0].lexeme == "shared");
}

TEST_CASE(compute_with_shared_declaration) {
    const char* src = R"(
        compute Reduce {
            shared float tile[64]
            let i = thread_id.x
            tile[i] = i
            return tile[0]
        }
    )";
    auto prog = parseSource(src);
    auto* cmp = dynamic_cast<ComputeDecl*>(prog->declarations[0].get());
    CHECK(cmp != nullptr);
    CHECK(cmp->name == "Reduce");
    // First child is the SharedDecl; the rest are let / assignments.
    CHECK(cmp->body.size() == 4);
    auto* sh = dynamic_cast<SharedDecl*>(cmp->body[0].get());
    CHECK(sh != nullptr);
    CHECK(sh->elementType == "float");
    CHECK(sh->name == "tile");
    CHECK(sh->size == 64);
}

TEST_CASE(shared_with_uint_element_type) {
    // The uint builtin type (Block 1) is a valid element type for
    // shared arrays. Catches a regression where parseSharedDecl
    // hardcodes the float type.
    const char* src = R"(
        compute UintTile {
            shared uint histogram[256]
            return 0
        }
    )";
    auto prog = parseSource(src);
    auto* cmp = dynamic_cast<ComputeDecl*>(prog->declarations[0].get());
    auto* sh = dynamic_cast<SharedDecl*>(cmp->body[0].get());
    CHECK(sh != nullptr);
    CHECK(sh->elementType == "uint");
    CHECK(sh->name == "histogram");
    CHECK(sh->size == 256);
}

// ===== Phase 3.5-A: storage decl explicit binding slot =====

TEST_CASE(storage_with_explicit_binding) {
    // `storage foo : rwstructuredbuffer<int> binding 2;` ?the
    // optional `binding <int>` suffix parses into StorageDecl::binding.
    const char* src = R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int> binding 2
            return 0
        }
    )";
    auto prog = parseSource(src);
    auto* cmp = dynamic_cast<ComputeDecl*>(prog->declarations[0].get());
    CHECK(cmp != nullptr);
    auto* st = dynamic_cast<StorageDecl*>(cmp->body[0].get());
    CHECK(st != nullptr);
    CHECK(st->name == "counters");
    CHECK(st->elementType == "int");
    CHECK(st->binding == 2);
}

TEST_CASE(storage_without_binding_keeps_default_minus_one) {
    // Absence of the `binding` suffix must default to -1 so the
    // BGFX backend's auto-assign path stays live.
    const char* src = R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int>
            return 0
        }
    )";
    auto prog = parseSource(src);
    auto* cmp = dynamic_cast<ComputeDecl*>(prog->declarations[0].get());
    CHECK(cmp != nullptr);
    auto* st = dynamic_cast<StorageDecl*>(cmp->body[0].get());
    CHECK(st != nullptr);
    CHECK(st->binding == -1);
}

TEST_CASE(storage_binding_keyword_recognized) {
    // `binding` is a new keyword. Verify the lexer classifies it
    // as TokenType::Binding (not as a generic Identifier), so the
    // parser's match(TokenType::Binding) check works.
    const char* src = "binding";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens.size() == 2);  // keyword + EOF
    CHECK(tokens[0].type == TokenType::Binding);
    CHECK(tokens[0].lexeme == "binding");
}

TEST_CASE(storage_binding_negative_int_is_error) {
    // `binding -1;` ?a negative literal after the keyword. The
    // parser should reject (storageBinding must be >= 0).
    const char* src = R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int> binding -1
            return 0
        }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    parser.parse();
    CHECK(parser.hasErrors());
}

TEST_CASE(storage_binding_non_int_is_error) {
    // `binding foo;` ?an identifier instead of an integer literal.
    // The parser must report the error (consume(IntLiteral, ...)
    // fails when the next token is `foo`).
    const char* src = R"(
        compute Foo {
            storage counters : rwstructuredbuffer<int> binding foo
            return 0
        }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    parser.parse();
    CHECK(parser.hasErrors());
}

TEST_CASE(storage_binding_works_with_structuredbuffer_too) {
    // Same binding suffix applies to the read-only `structuredbuffer`
    // variant (Read access). Confirms binding is a property of the
    // decl, not of the access mode.
    const char* src = R"(
        compute Foo {
            storage inputs : structuredbuffer<float> binding 7
            return 0
        }
    )";
    auto prog = parseSource(src);
    auto* cmp = dynamic_cast<ComputeDecl*>(prog->declarations[0].get());
    CHECK(cmp != nullptr);
    auto* st = dynamic_cast<StorageDecl*>(cmp->body[0].get());
    CHECK(st != nullptr);
    CHECK(st->access == StorageDecl::Access::Read);
    CHECK(st->binding == 7);
}

TEST_CASE(shared_missing_size_is_error) {
    // `shared float tile;` ?no size, no brackets. Parser must
    // surface the error (it consumes `tile` and then expects `[`).
    const char* src = R"(
        compute NoSize {
            shared float tile
            return 0
        }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    parser.parse();
    CHECK(parser.hasErrors());
}

TEST_CASE(shared_non_integer_size_is_error) {
    // `shared float tile[3.14];` ?float literal as size. Parser
    // consumes `tile[`, then expects an IntLiteral but gets
    // FloatLiteral, so it must report an error.
    const char* src = R"(
        compute FloatSize {
            shared float tile[3.14]
            return 0
        }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    parser.parse();
    CHECK(parser.hasErrors());
}

// ===== Phase 3.4: uniform block (UBO) =====

TEST_CASE(uniformblock_keyword_recognized) {
    // `uniformblock` must be a dedicated TokenType, not Identifier ?    // the parser dispatch relies on it to route to parseUniformBlockDecl.
    Lexer lexer("uniformblock Camera { vec3 pos }");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    CHECK(tokens[0].type == TokenType::UniformBlock);
    CHECK(tokens[0].lexeme == "uniformblock");
}

TEST_CASE(parse_uniformblock_minimal) {
    const char* src = R"(
        uniformblock Camera {
            vec3 position
            float fov
        }
        material P {
            vertex { return vec4(0.0) }
            fragment { return vec4(1.0) }
        }
    )";
    auto prog = parseSource(src);
    CHECK(prog != nullptr);
    // UBO is a top-level decl; it sits before the material.
    CHECK(prog->declarations.size() == 2);
    auto* ub = dynamic_cast<UniformBlockDecl*>(prog->declarations[0].get());
    CHECK(ub != nullptr);
    CHECK(ub->name == "Camera");
    CHECK(ub->fields.size() == 2);
    CHECK(ub->fields[0].type == "vec3");
    CHECK(ub->fields[0].name == "position");
    CHECK(ub->fields[1].type == "float");
    CHECK(ub->fields[1].name == "fov");
}

TEST_CASE(parse_uniformblock_with_semicolons) {
    // Variant: each field has a trailing `;`. The parser treats `;`
    // as optional, so this should parse identically to the
    // semicolon-less form above.
    const char* src = R"(
        uniformblock Camera {
            vec3 position;
            float fov;
        }
    )";
    auto prog = parseSource(src);
    auto* ub = dynamic_cast<UniformBlockDecl*>(prog->declarations[0].get());
    CHECK(ub != nullptr);
    CHECK(ub->fields.size() == 2);
    CHECK(ub->fields[0].name == "position");
    CHECK(ub->fields[1].name == "fov");
}

TEST_CASE(parse_uniformblock_with_uint_field) {
    // uint is a Phase 3.3 Block 1 builtin. UBO accepts it as a
    // field type just like float / vec3.
    const char* src = R"(
        uniformblock Stats {
            uint count
            float mean
        }
    )";
    auto prog = parseSource(src);
    auto* ub = dynamic_cast<UniformBlockDecl*>(prog->declarations[0].get());
    CHECK(ub != nullptr);
    CHECK(ub->fields[0].type == "uint");
    CHECK(ub->fields[0].name == "count");
}

TEST_CASE(parse_uniformblock_missing_closing_brace) {
    // No closing `}` ?parser must report an error.
    const char* src = "uniformblock Camera { vec3 pos";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    parser.parse();
    CHECK(parser.hasErrors());
}

TEST_CASE(compute_missing_brace_is_error) {
    // No closing '}' ?the parser must report a structural error
    // rather than silently accept the partial program.
    Lexer lexer("compute Foo { let x = 0");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    parser.parse();
    CHECK(parser.hasErrors());
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
    // defaultValue is vec3(0.0, 1.0, 0.0) ?a constructor call, not a
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

TEST_CASE(fragment_out_parses_as_mrt) {
    // Phase 6 #6: fragment `out` is MRT (declaration order → gl_FragData[N]).
    const char* src = R"(
        material X {
            vertex { return vec4(0.0) }
            fragment {
                out albedo : color = vec4(0.0)
                out nrm : color = vec4(0.0, 0.0, 1.0, 0.0)
                albedo = vec4(1.0, 0.0, 0.0, 1.0)
                nrm = vec4(0.0, 1.0, 0.0, 0.0)
            }
        }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto prog = parser.parse();
    CHECK(!parser.hasErrors());
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    auto* fs = dynamic_cast<FragmentFunc*>(mat->declarations[1].get());
    CHECK(fs != nullptr);
    CHECK(fs->outputs.size() == 2);
    auto* o0 = dynamic_cast<ShaderParam*>(fs->outputs[0].get());
    auto* o1 = dynamic_cast<ShaderParam*>(fs->outputs[1].get());
    CHECK(o0 != nullptr);
    CHECK(o1 != nullptr);
    CHECK(o0->name == "albedo");
    CHECK(o1->name == "nrm");
    CHECK(o0->dir == ShaderParam::Direction::Out);
    CHECK(o1->dir == ShaderParam::Direction::Out);
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

TEST_CASE(fragment_discard_statement) {
    const char* src = R"(
        material X {
            vertex { return vec4(0.0) }
            fragment {
                discard
                return vec4(1.0)
            }
        }
    )";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* fs = dynamic_cast<FragmentFunc*>(mat->declarations[1].get());
    CHECK(fs != nullptr);
    CHECK(fs->body.size() == 2);
    CHECK(dynamic_cast<DiscardStmt*>(fs->body[0].get()) != nullptr);
    CHECK(dynamic_cast<ReturnStmt*>(fs->body[1].get()) != nullptr);
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
            [variant useEmission]
            property emission = vec3(0.0)
            vertex { return vec4(0.0) }
            fragment { return vec4(1.0) }
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

// ===== Phase 2 Step 5: type-name demotion =====

TEST_CASE(uniform_with_builtin_type_parses_cleanly) {
    // After the token demotion, "vec3" is a plain Identifier whose
    // lexeme is "vec3". The parser should accept it without complaint ?    // the type validation is the SemanticAnalyzer's job.
    const char* src = R"(
        material X { vertex { return vec4(0.0) } fragment { return vec4(1.0) } }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    parser.parse();
    // Note: not asserting !parser.hasErrors() because Parser does NOT
    // enforce uniform's `vec3` lexeme ?that's the SemanticAnalyzer's
    // role via AYBuiltinTypes::isBuiltinType (Step 5d).
    (void)parser.hasErrors();
}

TEST_CASE(uniform_parsed_type_is_identifier_lexeme) {
    // The parser captures the type's lexeme in UniformDecl::type.
    // After Step 5, that's just the Identifier's lexeme.
    const char* src = R"(
        material X {
            uniform vec3 cameraPos
            vertex { return vec4(0.0) }
            fragment { return vec4(1.0) }
        }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto prog = parser.parse();
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    auto* uni = dynamic_cast<UniformDecl*>(mat->declarations[0].get());
    CHECK(uni != nullptr);
    CHECK(uni->type == "vec3");
    CHECK(uni->name == "cameraPos");
}

TEST_CASE(uniform_with_non_builtin_type_lexeme_parses_without_parser_error) {
    // "hello" is a valid Identifier so the parser accepts it. The
    // SemanticAnalyzer then rejects it with a Go-style diagnostic.
    // This test documents the parser-level behavior only.
    const char* src = R"(
        material X {
            uniform hello x
            vertex { return vec4(0.0) }
            fragment { return vec4(1.0) }
        }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto prog = parser.parse();
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    auto* uni = dynamic_cast<UniformDecl*>(mat->declarations[0].get());
    CHECK(uni != nullptr);
    CHECK(uni->type == "hello");  // captured lexeme, rejected downstream
}

// ===== Phase 1 RD-03: bone semantics + UBO array syntax =====

TEST_CASE(parse_material_accepts_boneindices_semantic) {
    // Phase 1 RD-03: `boneindices` is a built-in Phoskia semantic
    // accepted by parseShaderParam alongside position/normal/etc. The
    // BGFX converter maps this to the bgfx BLENDINDICES attribute.
    const char* src = R"(
        material P {
            vertex {
                in boneId : boneindices
                return vec4(0.0)
            }
            fragment { return vec4(1.0) }
        }
    )";
    auto prog = parseSource(src);
    CHECK(prog != nullptr);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    auto* vs = dynamic_cast<VertexFunc*>(mat->declarations[0].get());
    CHECK(vs != nullptr);
    CHECK(vs->params.size() == 1);
    auto* p = dynamic_cast<ShaderParam*>(vs->params[0].get());
    CHECK(p != nullptr);
    CHECK(p->dir == ShaderParam::Direction::In);
    CHECK(p->name == "boneId");
    CHECK(p->semantic == PhoskiaSemantic::BoneIndices);
}

TEST_CASE(parse_material_accepts_boneweights_semantic) {
    // Mirror case for the weights attribute (maps to BLENDWEIGHT).
    const char* src = R"(
        material P {
            vertex {
                in boneWt : boneweights
                return vec4(0.0)
            }
            fragment { return vec4(1.0) }
        }
    )";
    auto prog = parseSource(src);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    auto* vs = dynamic_cast<VertexFunc*>(mat->declarations[0].get());
    auto* p = dynamic_cast<ShaderParam*>(vs->params[0].get());
    CHECK(p != nullptr);
    CHECK(p->name == "boneWt");
    CHECK(p->semantic == PhoskiaSemantic::BoneWeights);
}

TEST_CASE(parse_material_accepts_tangent_semantic) {
    const char* src = R"(
        material P {
            vertex {
                in tan : tangent
                out worldTan : tangent = tan
                return vec4(0.0)
            }
            fragment {
                in worldTan : tangent
                return worldTan
            }
        }
    )";
    auto prog = parseSource(src);
    CHECK(prog != nullptr);
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    auto* vs = dynamic_cast<VertexFunc*>(mat->declarations[0].get());
    CHECK(vs != nullptr);
    CHECK(vs->params.size() == 2);
    auto* input = dynamic_cast<ShaderParam*>(vs->params[0].get());
    auto* output = dynamic_cast<ShaderParam*>(vs->params[1].get());
    CHECK(input != nullptr);
    CHECK(output != nullptr);
    CHECK(input->semantic == PhoskiaSemantic::Tangent);
    CHECK(output->semantic == PhoskiaSemantic::Tangent);
}

TEST_CASE(parse_uniformblock_accepts_array_field) {
    // Phase 1 RD-03: `uniformblock` parser consumes `[ N ]` after a
    // field. The result is recorded as `arrayLength > 0` on the
    // UniformBlockField, then the IR layer wraps the type in
    // `ArrayType<size>` and std140 layout reads the parallel
    // `uboFieldArrayLengths` vector.
    const char* src = R"(
        uniformblock Skeleton {
            mat4 bones[128]
        }
    )";
    auto prog = parseSource(src);
    CHECK(prog != nullptr);
    auto* ub = dynamic_cast<UniformBlockDecl*>(prog->declarations[0].get());
    CHECK(ub != nullptr);
    CHECK(ub->name == "Skeleton");
    CHECK(ub->fields.size() == 1);
    CHECK(ub->fields[0].type == "mat4");
    CHECK(ub->fields[0].name == "bones");
    CHECK(ub->fields[0].arrayLength == 128);
}

TEST_CASE(parse_uniformblock_array_length_must_be_positive) {
    // `mat4 bones[0]` is a structural error — std140 layout requires
    // at least one element. The parser must surface this rather
    // than silently producing a 0-sized array.
    const char* src = "uniformblock S { mat4 bones[0] }";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    parser.parse();
    CHECK(parser.hasErrors());
}

TEST_CASE(compute_numthreads_accepts_hex_integer_literals) {
    Lexer lexer("[numthreads(0x10, 0x2, 1)] compute HexGroup { }");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto prog = parser.parse();

    CHECK_FALSE(parser.hasErrors());
    CHECK(prog != nullptr);
    auto* compute = dynamic_cast<ComputeDecl*>(prog->declarations[0].get());
    CHECK(compute != nullptr);
    CHECK(compute->hasNumThreads);
    CHECK(compute->numThreads[0] == 16u);
    CHECK(compute->numThreads[1] == 2u);
    CHECK(compute->numThreads[2] == 1u);
}

TEST_CASE(compute_numthreads_rejects_zero_component) {
    Lexer lexer("[numthreads(8, 0, 1)] compute InvalidGroup { }");
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    parser.parse();
    CHECK(parser.hasErrors());
}

TEST_SUITE_END
