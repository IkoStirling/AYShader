// ============================================================
// AYShader TypeInference Unit Tests (Phase 2 Step 2)
// ============================================================
//
// Exercises AYTypeInference::infer() directly. Each test builds a small
// AST by hand (or via the parser), wraps it in a TypeInference engine,
// and asserts the resulting Type.

#include "AYTypeInference.h"
#include "AYType.h"
#include "AYAst.h"
#include "AYLexer.h"
#include "AYParser.h"
#include "AYBuiltinFunctions.h"
#include "AYTest.h"

#include <memory>

using namespace ayt::shader::phoskia;

namespace {

// Helper: build a TypeInference pre-loaded with every registered builtin
// function so identifier lookups for `vec3`, `normalize`, `sample`, etc.
// resolve correctly.
struct InferenceEnv {
    TypeEnvironment env;
    TypeInference inference;
    InferenceEnv() : inference(env) {
        for (const auto& name : BuiltinFunctionRegistry::instance().getAllFunctionNames()) {
            auto func = BuiltinFunctionRegistry::instance().getFunction(name);
            if (func) {
                env.addFunction(name,
                    std::make_shared<FunctionType>(func->paramTypes, func->returnType));
            }
        }
    }
};

// Convenience: cast helper — extract the concrete type a TypeVar points to.
std::shared_ptr<Type> resolve(std::shared_ptr<Type> t) {
    while (auto tv = std::dynamic_pointer_cast<TypeVar>(t)) {
        if (tv->hasSolution()) t = tv->getSolution();
        else break;
    }
    return t;
}

}  // namespace

TEST_SUITE(TypeInferenceTests)

// ===== Literal inference =====

TEST_CASE(literal_float_is_float) {
    InferenceEnv e;
    LiteralExpr lit(1.5f);
    auto t = e.inference.infer(lit);
    CHECK(t->equals(*BuiltinTypes::Float));
}

TEST_CASE(literal_int_is_int) {
    InferenceEnv e;
    LiteralExpr lit(42);
    auto t = e.inference.infer(lit);
    CHECK(t->equals(*BuiltinTypes::Int));
}

TEST_CASE(literal_bool_is_bool) {
    InferenceEnv e;
    LiteralExpr lit(true);
    auto t = e.inference.infer(lit);
    CHECK(t->equals(*BuiltinTypes::Bool));
}

// ===== Identifier resolution =====

TEST_CASE(identifier_known_in_env) {
    InferenceEnv e;
    e.env.addVariable("speed", BuiltinTypes::Float);
    IdentifierExpr id("speed");
    auto t = resolve(e.inference.infer(id));
    CHECK(t->equals(*BuiltinTypes::Float));
}

TEST_CASE(identifier_unknown_returns_fresh_typevar) {
    InferenceEnv e;
    IdentifierExpr id("nonexistent");
    auto t = e.inference.infer(id);
    // Unknown identifiers produce a fresh TypeVar (Dynamic placeholder);
    // the analyzer layer surfaces the "undefined identifier" error.
    CHECK(t != nullptr);
}

// ===== Binary expression unification =====

TEST_CASE(binary_arithmetic_unifies_operands) {
    InferenceEnv e;
    auto lit = std::make_unique<LiteralExpr>(1.0f);
    auto lit2 = std::make_unique<LiteralExpr>(2.0f);
    Token op; op.type = TokenType::Plus;
    BinaryExpr bin(std::move(lit), op, std::move(lit2));
    auto t = resolve(e.inference.infer(bin));
    CHECK(t->equals(*BuiltinTypes::Float));
}

// ===== Scalar×vector broadcasting (commutative) =====
//
// GLSL supports component-wise arithmetic between a scalar and a
// vector in either operand position: `vec3 * float` and
// `float * vec3` both yield vec3 (per-component multiply). The
// same applies to `+` / `-` / `/`. Phoskia's TypeInference mirrors
// this: the result of a scalar×vector op is the vector side, no
// matter which side the vector sits on. These tests pin the
// behaviour so a refactor that breaks commutativity (e.g. always
// picking the left side) is caught.

namespace {
// Build a vec3 CallExpr with three float literal components.
std::unique_ptr<Expr> mkVec3(float x, float y, float z) {
    std::vector<ExprPtr> args;
    args.push_back(std::make_unique<LiteralExpr>(x));
    args.push_back(std::make_unique<LiteralExpr>(y));
    args.push_back(std::make_unique<LiteralExpr>(z));
    return std::make_unique<CallExpr>(
        std::make_unique<IdentifierExpr>("vec3"), std::move(args));
}
// Build a vec4 CallExpr.
std::unique_ptr<Expr> mkVec4(float x, float y, float z, float w) {
    std::vector<ExprPtr> args;
    args.push_back(std::make_unique<LiteralExpr>(x));
    args.push_back(std::make_unique<LiteralExpr>(y));
    args.push_back(std::make_unique<LiteralExpr>(z));
    args.push_back(std::make_unique<LiteralExpr>(w));
    return std::make_unique<CallExpr>(
        std::make_unique<IdentifierExpr>("vec4"), std::move(args));
}
std::unique_ptr<Expr> mkFloat(float v) {
    return std::make_unique<LiteralExpr>(v);
}
Token mkOp(TokenType tt) { Token op; op.type = tt; return op; }
}  // namespace

TEST_CASE(scalar_times_vec3_returns_vec3) {
    InferenceEnv e;
    BinaryExpr bin(mkFloat(2.0f), mkOp(TokenType::Star), mkVec3(1, 2, 3));
    auto t = resolve(e.inference.infer(bin));
    CHECK(t->equals(*BuiltinTypes::Vec3()));
}

TEST_CASE(vec3_times_scalar_returns_vec3) {
    InferenceEnv e;
    BinaryExpr bin(mkVec3(1, 2, 3), mkOp(TokenType::Star), mkFloat(2.0f));
    auto t = resolve(e.inference.infer(bin));
    CHECK(t->equals(*BuiltinTypes::Vec3()));
}

TEST_CASE(scalar_plus_vec3_returns_vec3) {
    InferenceEnv e;
    BinaryExpr bin(mkFloat(1.0f), mkOp(TokenType::Plus), mkVec3(2, 3, 4));
    auto t = resolve(e.inference.infer(bin));
    CHECK(t->equals(*BuiltinTypes::Vec3()));
}

TEST_CASE(vec3_minus_scalar_returns_vec3) {
    InferenceEnv e;
    BinaryExpr bin(mkVec3(2, 3, 4), mkOp(TokenType::Minus), mkFloat(1.0f));
    auto t = resolve(e.inference.infer(bin));
    CHECK(t->equals(*BuiltinTypes::Vec3()));
}

TEST_CASE(scalar_over_vec3_returns_vec3) {
    InferenceEnv e;
    BinaryExpr bin(mkFloat(6.0f), mkOp(TokenType::Slash), mkVec3(2, 3, 6));
    auto t = resolve(e.inference.infer(bin));
    CHECK(t->equals(*BuiltinTypes::Vec3()));
}

TEST_CASE(vec3_over_scalar_returns_vec3) {
    InferenceEnv e;
    BinaryExpr bin(mkVec3(2, 3, 6), mkOp(TokenType::Slash), mkFloat(2.0f));
    auto t = resolve(e.inference.infer(bin));
    CHECK(t->equals(*BuiltinTypes::Vec3()));
}

TEST_CASE(scalar_times_vec4_returns_vec4) {
    InferenceEnv e;
    BinaryExpr bin(mkFloat(0.5f), mkOp(TokenType::Star), mkVec4(1, 1, 1, 1));
    auto t = resolve(e.inference.infer(bin));
    CHECK(t->equals(*BuiltinTypes::Vec4()));
}

TEST_CASE(vec4_times_scalar_returns_vec4) {
    InferenceEnv e;
    BinaryExpr bin(mkVec4(1, 1, 1, 1), mkOp(TokenType::Star), mkFloat(0.5f));
    auto t = resolve(e.inference.infer(bin));
    CHECK(t->equals(*BuiltinTypes::Vec4()));
}

// Chained scalar×vector (e.g. PBR `D * G * F / max(4*NdotV, 0.001)`):
// the result of each step is the wider type, and the next op
// broadcasts from the other side again.
TEST_CASE(chained_scalar_vector_vector_returns_vector) {
    InferenceEnv e;
    // ((float * vec3) * vec3) — left-assoc parse.
    auto inner = std::make_unique<BinaryExpr>(
        mkFloat(2.0f), mkOp(TokenType::Star), mkVec3(1, 1, 1));
    BinaryExpr outer(std::move(inner), mkOp(TokenType::Star), mkVec3(3, 3, 3));
    auto t = resolve(e.inference.infer(outer));
    CHECK(t->equals(*BuiltinTypes::Vec3()));
}

TEST_CASE(binary_comparison_returns_bool) {
    InferenceEnv e;
    auto a = std::make_unique<LiteralExpr>(1.0f);
    auto b = std::make_unique<LiteralExpr>(2.0f);
    Token op; op.type = TokenType::Less;
    BinaryExpr lt(std::move(a), op, std::move(b));
    auto t = resolve(e.inference.infer(lt));
    CHECK(t->equals(*BuiltinTypes::Bool));
}

TEST_CASE(binary_logical_returns_bool) {
    InferenceEnv e;
    auto a = std::make_unique<LiteralExpr>(true);
    auto b = std::make_unique<LiteralExpr>(false);
    Token op; op.type = TokenType::And;
    BinaryExpr andExpr(std::move(a), op, std::move(b));
    auto t = resolve(e.inference.infer(andExpr));
    CHECK(t->equals(*BuiltinTypes::Bool));
}

// ===== Unary expression =====

TEST_CASE(unary_minus_preserves_type) {
    InferenceEnv e;
    auto lit = std::make_unique<LiteralExpr>(3.14f);
    Token op; op.type = TokenType::Minus;
    UnaryExpr neg(op, std::move(lit));
    auto t = resolve(e.inference.infer(neg));
    CHECK(t->equals(*BuiltinTypes::Float));
}

// ===== Swizzle inference =====

TEST_CASE(swizzle_single_axis_returns_float) {
    InferenceEnv e;
    e.env.addVariable("v", BuiltinTypes::Vec3());
    auto obj = std::make_unique<IdentifierExpr>("v");
    MemberExpr m(std::move(obj), "x");
    auto t = resolve(e.inference.infer(m));
    CHECK(t->equals(*BuiltinTypes::Float));
}

TEST_CASE(swizzle_triple_axis_returns_vec3) {
    InferenceEnv e;
    e.env.addVariable("c", BuiltinTypes::Vec4());
    auto obj = std::make_unique<IdentifierExpr>("c");
    MemberExpr m(std::move(obj), "rgb");
    auto t = resolve(e.inference.infer(m));
    auto v3 = BuiltinTypes::Vec3();
    CHECK(t->equals(*v3));
}

TEST_CASE(swizzle_quad_returns_vec4) {
    InferenceEnv e;
    e.env.addVariable("v", BuiltinTypes::Vec4());
    auto obj = std::make_unique<IdentifierExpr>("v");
    MemberExpr m(std::move(obj), "rgba");
    auto t = resolve(e.inference.infer(m));
    auto v4 = BuiltinTypes::Vec4();
    CHECK(t->equals(*v4));
}

TEST_CASE(swizzle_duplicate_chars_returns_vec) {
    InferenceEnv e;
    e.env.addVariable("v", BuiltinTypes::Vec4());
    auto obj = std::make_unique<IdentifierExpr>("v");
    MemberExpr m(std::move(obj), "rrgg");
    auto t = resolve(e.inference.infer(m));
    auto v4 = BuiltinTypes::Vec4();
    CHECK(t->equals(*v4));
}

// ===== Index inference =====

TEST_CASE(index_into_vec3_returns_float) {
    InferenceEnv e;
    e.env.addVariable("v", BuiltinTypes::Vec3());
    auto obj = std::make_unique<IdentifierExpr>("v");
    auto idx = std::make_unique<LiteralExpr>(0);
    IndexExpr ix(std::move(obj), std::move(idx));
    auto t = resolve(e.inference.infer(ix));
    CHECK(t->equals(*BuiltinTypes::Float));
}

TEST_CASE(index_into_vec4_returns_float) {
    InferenceEnv e;
    e.env.addVariable("v", BuiltinTypes::Vec4());
    auto obj = std::make_unique<IdentifierExpr>("v");
    auto idx = std::make_unique<LiteralExpr>(3);
    IndexExpr ix(std::move(obj), std::move(idx));
    auto t = resolve(e.inference.infer(ix));
    CHECK(t->equals(*BuiltinTypes::Float));
}

// ===== Built-in call inference =====

TEST_CASE(call_normalize_vec3_returns_vec3) {
    InferenceEnv e;
    e.env.addVariable("v", BuiltinTypes::Vec3());
    auto callee = std::make_unique<IdentifierExpr>("normalize");
    std::vector<ExprPtr> args;
    args.push_back(std::make_unique<IdentifierExpr>("v"));
    CallExpr call(std::move(callee), std::move(args));
    auto t = resolve(e.inference.infer(call));
    auto v3 = BuiltinTypes::Vec3();
    CHECK(t->equals(*v3));
}

TEST_CASE(call_dot_returns_float) {
    InferenceEnv e;
    e.env.addVariable("a", BuiltinTypes::Vec3());
    e.env.addVariable("b", BuiltinTypes::Vec3());
    auto callee = std::make_unique<IdentifierExpr>("dot");
    std::vector<ExprPtr> args;
    args.push_back(std::make_unique<IdentifierExpr>("a"));
    args.push_back(std::make_unique<IdentifierExpr>("b"));
    CallExpr call(std::move(callee), std::move(args));
    auto t = resolve(e.inference.infer(call));
    CHECK(t->equals(*BuiltinTypes::Float));
}

TEST_CASE(call_length_returns_float) {
    InferenceEnv e;
    e.env.addVariable("v", BuiltinTypes::Vec3());
    auto callee = std::make_unique<IdentifierExpr>("length");
    std::vector<ExprPtr> args;
    args.push_back(std::make_unique<IdentifierExpr>("v"));
    CallExpr call(std::move(callee), std::move(args));
    auto t = resolve(e.inference.infer(call));
    CHECK(t->equals(*BuiltinTypes::Float));
}

TEST_CASE(call_cross_returns_vec3) {
    InferenceEnv e;
    e.env.addVariable("a", BuiltinTypes::Vec3());
    e.env.addVariable("b", BuiltinTypes::Vec3());
    auto callee = std::make_unique<IdentifierExpr>("cross");
    std::vector<ExprPtr> args;
    args.push_back(std::make_unique<IdentifierExpr>("a"));
    args.push_back(std::make_unique<IdentifierExpr>("b"));
    CallExpr call(std::move(callee), std::move(args));
    auto t = resolve(e.inference.infer(call));
    auto v3 = BuiltinTypes::Vec3();
    CHECK(t->equals(*v3));
}

TEST_CASE(call_sample_returns_vec4) {
    InferenceEnv e;
    e.env.addVariable("tex", BuiltinTypes::Dynamic);  // texture: opaque
    e.env.addVariable("uv", BuiltinTypes::Vec2());
    auto callee = std::make_unique<IdentifierExpr>("sample");
    std::vector<ExprPtr> args;
    args.push_back(std::make_unique<IdentifierExpr>("tex"));
    args.push_back(std::make_unique<IdentifierExpr>("uv"));
    CallExpr call(std::move(callee), std::move(args));
    auto t = resolve(e.inference.infer(call));
    auto v4 = BuiltinTypes::Vec4();
    CHECK(t->equals(*v4));
}

// ===== Type constructor inference =====

TEST_CASE(vec3_constructor_from_three_floats) {
    InferenceEnv e;
    auto callee = std::make_unique<IdentifierExpr>("vec3");
    std::vector<ExprPtr> args;
    args.push_back(std::make_unique<LiteralExpr>(1.0f));
    args.push_back(std::make_unique<LiteralExpr>(2.0f));
    args.push_back(std::make_unique<LiteralExpr>(3.0f));
    CallExpr call(std::move(callee), std::move(args));
    auto t = resolve(e.inference.infer(call));
    auto v3 = BuiltinTypes::Vec3();
    CHECK(t->equals(*v3));
}

TEST_CASE(vec4_constructor_from_vec3_and_float) {
    InferenceEnv e;
    e.env.addVariable("v3", BuiltinTypes::Vec3());
    auto callee = std::make_unique<IdentifierExpr>("vec4");
    std::vector<ExprPtr> args;
    args.push_back(std::make_unique<IdentifierExpr>("v3"));
    args.push_back(std::make_unique<LiteralExpr>(1.0f));
    CallExpr call(std::move(callee), std::move(args));
    auto t = resolve(e.inference.infer(call));
    auto v4 = BuiltinTypes::Vec4();
    CHECK(t->equals(*v4));
}

TEST_CASE(mat4_constructor_from_four_vec4) {
    InferenceEnv e;
    e.env.addVariable("c0", BuiltinTypes::Vec4());
    e.env.addVariable("c1", BuiltinTypes::Vec4());
    e.env.addVariable("c2", BuiltinTypes::Vec4());
    e.env.addVariable("c3", BuiltinTypes::Vec4());
    auto callee = std::make_unique<IdentifierExpr>("mat4");
    std::vector<ExprPtr> args;
    args.push_back(std::make_unique<IdentifierExpr>("c0"));
    args.push_back(std::make_unique<IdentifierExpr>("c1"));
    args.push_back(std::make_unique<IdentifierExpr>("c2"));
    args.push_back(std::make_unique<IdentifierExpr>("c3"));
    CallExpr call(std::move(callee), std::move(args));
    auto t = resolve(e.inference.infer(call));
    auto m4 = BuiltinTypes::Mat4();
    CHECK(t->equals(*m4));
}

// ===== End-to-end via parser =====

TEST_CASE(inferred_return_type_via_parser_for_vec4) {
    const char* src = R"(
        material X {
            vertex { return vec4(1.0, 0.0, 0.0, 1.0) }
            fragment { return vec4(1.0) }
        }
    )";
    Lexer lexer(src);
    std::vector<Token> tokens;
    lexer.tokenize(tokens);
    Parser parser(tokens);
    auto prog = parser.parse();

    InferenceEnv e;
    auto* mat = dynamic_cast<MaterialDecl*>(prog->declarations[0].get());
    CHECK(mat != nullptr);
    auto* vs = dynamic_cast<VertexFunc*>(mat->declarations[0].get());
    CHECK(vs != nullptr);
    auto* ret = dynamic_cast<ReturnStmt*>(vs->body[0].get());
    CHECK(ret != nullptr);
    auto t = resolve(e.inference.infer(*ret->value));
    auto v4 = BuiltinTypes::Vec4();
    CHECK(t->equals(*v4));
}

// ===== Unify mechanics =====

TEST_CASE(unify_two_concrete_floats_succeeds) {
    InferenceEnv e;
    CHECK(e.inference.unify(BuiltinTypes::Float, BuiltinTypes::Float));
}

TEST_CASE(unify_float_with_int_fails) {
    InferenceEnv e;
    CHECK_FALSE(e.inference.unify(BuiltinTypes::Float, BuiltinTypes::Int));
}

TEST_CASE(unify_typevar_with_concrete_binds_var) {
    InferenceEnv e;
    auto tv = std::make_shared<TypeVar>("a");
    CHECK(e.inference.unify(tv, BuiltinTypes::Float));
    CHECK(tv->hasSolution());
    CHECK(tv->getSolution()->equals(*BuiltinTypes::Float));
}

TEST_SUITE_END