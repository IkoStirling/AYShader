// AYTypeInference.cpp - Hindley-Milner type inference implementation

#include "AYShader/TypeInference.h"
#include "AYShader/BuiltinFunctions.h"
#include <stdexcept>

namespace ayt::shader::phoskia
{

namespace {
// Follow a TypeVar chain to its root (the last TypeVar, or a concrete
// type, or null). Detects cycles: if the chain loops back to a TypeVar
// we've already visited, return that visited TypeVar's solution as a
// dead-end so we don't recurse forever. Without this guard a sequence
// of unify(TypeVar, TypeVar) calls can build an arbitrarily deep
// chain that the recursive unify entry then walks one frame at a time
// until the stack blows (observed on PBR `pow5 = a*a*a*a*a`).
//
// Defined at the top of this TU so both inferBinaryExpr (below) and
// unify (further below) can call it without an extra forward
// declaration.
std::shared_ptr<Type> resolveTypeVar(std::shared_ptr<Type> t) {
    std::shared_ptr<Type> last = t;
    int hops = 0;
    while (auto tv = std::dynamic_pointer_cast<TypeVar>(last)) {
        if (!tv->hasSolution()) break;
        auto next = tv->getSolution();
        // Cycle detection — refuse to follow chains that loop back to
        // a TypeVar already on the path.
        if (next == last || next == t) return last;
        if (++hops > 64) return last;  // belt-and-suspenders depth cap
        last = next;
    }
    return last;
}
}  // namespace

std::shared_ptr<Type> TypeInference::infer(const Expr& expr) {
    if (auto binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        return inferBinaryExpr(*binary);
    }
    if (auto unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        return inferUnaryExpr(*unary);
    }
    if (auto call = dynamic_cast<const CallExpr*>(&expr)) {
        return inferCallExpr(*call);
    }
    if (auto id = dynamic_cast<const IdentifierExpr*>(&expr)) {
        return inferIdentifierExpr(*id);
    }
    if (auto lit = dynamic_cast<const LiteralExpr*>(&expr)) {
        return inferLiteralExpr(*lit);
    }
    if (auto member = dynamic_cast<const MemberExpr*>(&expr)) {
        return inferMemberExpr(*member);
    }
    if (auto index = dynamic_cast<const IndexExpr*>(&expr)) {
        return inferIndexExpr(*index);
    }
    return newTypeVar();
}

std::shared_ptr<Type> TypeInference::inferBinaryExpr(const BinaryExpr& expr) {
    auto leftType = infer(*expr.left);
    auto rightType = infer(*expr.right);

    // Check for type variables and unify
    auto resultType = newTypeVar();

    // Arithmetic operations unify operands and bind the result. We
    // support scalar×vector broadcasting (a Phoskia / GLSL
    // convention — `vec3 * float` returns `vec3`). The rule:
    //   1. If both sides have a concrete type, prefer the wider one
    //      (vector over scalar) for the result.
    //   2. If either side is an unresolved TypeVar, bind the result
    //      to whichever side has a concrete type; if both are
    //      unresolved, the result is left as a fresh TypeVar (the
    //      BGFX converter's let-stmt inference then defaults to
    //      float for GLSL emission).
    if (expr.op.type == TokenType::Plus || expr.op.type == TokenType::Minus ||
        expr.op.type == TokenType::Star || expr.op.type == TokenType::Slash) {
        // Scalar×vector broadcasting (GLSL): `vec3 * float` → vec3.
        // CRITICAL: do NOT unify(TypeVar, Float) first — that poisons a
        // still-unresolved left operand (e.g. `Lights.dirs[0].xyz` when
        // the UBO StructType was missing) into float, and the BGFX
        // emitter then prints `float L0 = ...` which kills Lambert.
        auto lConc = resolveTypeVar(leftType);
        auto rConc = resolveTypeVar(rightType);
        auto lv = std::dynamic_pointer_cast<VectorType>(lConc);
        auto rv = std::dynamic_pointer_cast<VectorType>(rConc);
        auto lVar = std::dynamic_pointer_cast<TypeVar>(lConc);
        auto rVar = std::dynamic_pointer_cast<TypeVar>(rConc);
        if (lv && !rv) {
            unify(resultType, leftType);
        } else if (rv && !lv) {
            unify(resultType, rightType);
        } else if (!lVar && !rVar) {
            unify(leftType, rightType);
            unify(resultType, leftType);
        } else if (!lVar) {
            unify(resultType, leftType);
        } else if (!rVar) {
            unify(resultType, rightType);
        }
        // else both unresolved TypeVars — leave resultType fresh.
    }
    // Comparison operations
    else if (expr.op.type == TokenType::EqualEqual || expr.op.type == TokenType::BangEqual ||
             expr.op.type == TokenType::Less || expr.op.type == TokenType::LessEqual ||
             expr.op.type == TokenType::Greater || expr.op.type == TokenType::GreaterEqual) {
        unify(resultType, BuiltinTypes::Bool);
    }
    // Logical operations
    else if (expr.op.type == TokenType::And || expr.op.type == TokenType::Or) {
        unify(leftType, BuiltinTypes::Bool);
        unify(rightType, BuiltinTypes::Bool);
        unify(resultType, BuiltinTypes::Bool);
    }

    return resultType;
}

std::shared_ptr<Type> TypeInference::inferUnaryExpr(const UnaryExpr& expr) {
    auto operandType = infer(*expr.operand);

    if (expr.op.type == TokenType::Minus) {
        auto resultType = newTypeVar();
        unify(resultType, operandType);
        return resultType;
    }
    if (expr.op.type == TokenType::Bang) {
        unify(operandType, BuiltinTypes::Bool);
        return BuiltinTypes::Bool;
    }

    return newTypeVar();
}

std::shared_ptr<Type> TypeInference::inferCallExpr(const CallExpr& expr) {
    auto funcType = infer(*expr.callee);

    // ---- Built-in function: vec2/vec3/vec4/ivec2..4/mat2..4/float/int/bool ----
    // These double as both ordinary functions (when registered as builtins)
    // AND as type constructors (vec3(vec4), vec4(vec3, float), ...).
    if (auto id = dynamic_cast<const IdentifierExpr*>(expr.callee.get())) {
        // Infer arguments first so same-arity overloads (mix float vs vec3,
        // normalize vec2 vs vec3, …) can be scored by concrete arg types.
        // Arity-only lookup previously always returned the first registered
        // mix(F,F,F) and emitted `float lit = mix(vec3, …)`.
        std::vector<std::shared_ptr<Type>> argTypes;
        argTypes.reserve(expr.args.size());
        for (const auto& a : expr.args) {
            argTypes.push_back(infer(*a));
        }

        const BuiltinFunction* builtin = nullptr;
        int bestScore = -1;
        if (auto* overloads =
                BuiltinFunctionRegistry::instance().getOverloads(id->name)) {
            for (const auto& ovl : *overloads) {
                if (ovl.paramTypes.size() != argTypes.size()) continue;
                int score = 0;
                bool ok = true;
                for (size_t i = 0; i < argTypes.size(); ++i) {
                    auto arg = resolveTypeVar(argTypes[i]);
                    const auto& param = ovl.paramTypes[i];
                    if (std::dynamic_pointer_cast<TypeVar>(arg)) {
                        // Unresolved — soft match; prefer overloads that
                        // still fit once other args pin the type.
                        score += 1;
                        continue;
                    }
                    if (param->kind() == TypeKind::Dynamic ||
                        arg->kind() == TypeKind::Dynamic) {
                        score += 3;
                        continue;
                    }
                    if (arg->equals(*param)) {
                        score += 10;
                        continue;
                    }
                    ok = false;
                    break;
                }
                if (!ok) continue;
                if (score > bestScore) {
                    bestScore = score;
                    builtin = &ovl;
                }
            }
        }

        if (builtin) {
            for (size_t i = 0; i < argTypes.size(); ++i) {
                unify(argTypes[i], builtin->paramTypes[i]);
            }
            return builtin->returnType;
        }

        // ---- Type constructor fallback (when not registered as builtin) ----
        // Phase 1 registers scalar/vector builtins via registerDefaults;
        // this branch handles arities those don't cover, e.g. vec4(v3, f),
        // mat4(v4, v4, v4, v4).
        const std::string& name = id->name;
        if (name == "vec2" || name == "vec3" || name == "vec4" ||
            name == "ivec2" || name == "ivec3" || name == "ivec4" ||
            name == "uvec2" || name == "uvec3" || name == "uvec4" ||
            name == "mat2" || name == "mat3" || name == "mat4") {
            auto inferred = inferConstructor(name, expr.args);
            if (inferred) return inferred;
        }

        // No matching builtin overload and not a type constructor.
        // Do NOT fall back to getFunction()'s first overload return
        // type (scalar mix→float poisoned vec2 mix lets as
        // `float _rectUv = mix(...)` when vec2 overloads were missing).
        return newTypeVar();
    }

    // ---- User-defined function: FunctionType in env ----
    if (auto func = dynamic_cast<FunctionType*>(funcType.get())) {
        if (expr.args.size() == func->params().size()) {
            for (size_t i = 0; i < expr.args.size(); ++i) {
                auto argType = infer(*expr.args[i]);
                unify(argType, func->params()[i]);
            }
            return func->returnType();
        }
        for (const auto& a : expr.args) infer(*a);
        return func->returnType();
    }

    return newTypeVar();
}

std::shared_ptr<Type> TypeInference::inferConstructor(
    const std::string& name,
    const std::vector<ExprPtr>& args) {
    // Count total components requested by the constructor name.
    auto vecFor = [&](int dim) -> std::shared_ptr<VectorType> {
        // Phase 3.3 Block 3: uvec2/3/4 prefix "uvec" — distinguished
        // from "ivec" by the second character ('v' vs 'e'). "vec"
        // defaults to float as before.
        PrimitiveType elem = PrimitiveType::Float;
        if (name.size() >= 4 && name[0] == 'i' && name[1] == 'v') elem = PrimitiveType::Int;
        else if (name.size() >= 4 && name[0] == 'u' && name[1] == 'v') elem = PrimitiveType::Uint;
        switch (dim) {
            case 2: return std::make_shared<VectorType>(elem, 2);
            case 3: return std::make_shared<VectorType>(elem, 3);
            case 4: return std::make_shared<VectorType>(elem, 4);
            default: return nullptr;
        }
    };
    auto matFor = [&](int dim) -> std::shared_ptr<MatrixType> {
        switch (dim) {
            case 2: return std::make_shared<MatrixType>(2, 2);
            case 3: return std::make_shared<MatrixType>(3, 3);
            case 4: return std::make_shared<MatrixType>(4, 4);
            default: return nullptr;
        }
    };

    int dim = 0;
    if (name[0] == 'm') dim = name[3] - '0';        // mat2/3/4
    else if (name[0] == 'i' || name[0] == 'u') dim = name[4] - '0';   // ivec/uvec + dim
    else dim = name[3] - '0';                       // vec2/3/4

    // vec/ivec: sum the component count of each argument.
    int total = 0;
    bool failed = false;
    for (const auto& a : args) {
        auto t = infer(*a);
        // Resolve any pending type variable.
        std::shared_ptr<Type> concrete = t;
        while (auto tv = std::dynamic_pointer_cast<TypeVar>(concrete)) {
            if (tv->hasSolution()) concrete = tv->getSolution();
            else break;
        }
        if (auto v = std::dynamic_pointer_cast<VectorType>(concrete)) {
            total += static_cast<int>(v->dimension());
        } else if (auto p = std::dynamic_pointer_cast<PrimitiveType_>(concrete)) {
            total += 1;
        } else {
            // Unknown arg shape — count as 1 so we still infer a result;
            // mismatches will be caught by unify at the caller's level.
            total += 1;
            failed = true;
        }
    }
    if (failed) return vecFor(dim);  // best-effort: return target dim

    if (name[0] == 'm') {
        // matN constructors take N vectors (columns).
        if (args.size() == static_cast<size_t>(dim)) {
            for (const auto& a : args) {
                auto t = infer(*a);
                std::shared_ptr<Type> concrete = t;
                while (auto tv = std::dynamic_pointer_cast<TypeVar>(concrete)) {
                    if (tv->hasSolution()) concrete = tv->getSolution();
                    else break;
                }
                if (auto v = std::dynamic_pointer_cast<VectorType>(concrete)) {
                    if (v->dimension() != static_cast<size_t>(dim)) {
                        return matFor(dim);  // mismatch — best effort
                    }
                }
            }
            return matFor(dim);
        }
        return matFor(dim);  // best effort
    }

    if (total == dim) return vecFor(dim);
    // Mismatched component count — fall through and return target dim.
    return vecFor(dim);
}

std::shared_ptr<Type> TypeInference::inferIdentifierExpr(const IdentifierExpr& expr) {
    auto type = _env.getVariable(expr.name);
    if (!type) {
        // Check built-in functions
        auto funcType = _env.getFunction(expr.name);
        if (funcType) return funcType;
        // Phase 3.2 Block 2: compute thread-id builtins (`thread_id` /
        // `group_id` / `dispatch_id`) are zero-arg functions but Phoskia
        // source uses them as bare identifiers (`thread_id.x` rather
        // than `thread_id().x`). When a bare identifier resolves to
        // one of these builtins, return the return type (uvec3 — Phase
        // 3.3 Block 3 strict-typed; previously vec3) so subsequent
        // member access (`thread_id.x`) type-checks correctly and the
        // swizzle result becomes `uint`. Without this, `inferMemberExpr`
        // would see the FunctionType wrapper and fail to resolve the
        // swizzle.
        auto builtin = BuiltinFunctionRegistry::instance().getFunction(expr.name);
        if (builtin && builtin->paramTypes.empty() && builtin->returnType) {
            return builtin->returnType;
        }
        return newTypeVar();
    }
    return type;
}

std::shared_ptr<Type> TypeInference::inferLiteralExpr(const LiteralExpr& expr) {
    return std::visit([](auto&& arg) -> std::shared_ptr<Type> {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, float>) {
            return BuiltinTypes::Float;
        } else if constexpr (std::is_same_v<T, int>) {
            return BuiltinTypes::Int;
        } else if constexpr (std::is_same_v<T, bool>) {
            return BuiltinTypes::Bool;
        } else if constexpr (std::is_same_v<T, std::string>) {
            return BuiltinTypes::String;
        }
        return BuiltinTypes::Dynamic;
    }, expr.value);
}

std::shared_ptr<Type> TypeInference::inferMemberExpr(const MemberExpr& expr) {
    auto objectType = infer(*expr.object);

    // Resolve any pending type variable so we can introspect the swizzle.
    std::shared_ptr<Type> concrete = objectType;
    while (auto tv = std::dynamic_pointer_cast<TypeVar>(concrete)) {
        if (tv->hasSolution()) {
            concrete = tv->getSolution();
        } else {
            break;
        }
    }

    // Swizzle on a vector: c.r → float, v.rgb → vec3, v.rrgg → vec4, v.xyzw → vec4
    if (auto vec = std::dynamic_pointer_cast<VectorType>(concrete)) {
        const std::string& m = expr.member;
        // Each swizzle character must map to an existing axis.
        // Allowed axes: x/y/z/w, r/g/b/a — same character count, same dimension.
        // We just require the dimension of the swizzle result matches the
        // length of the swizzle string. Single-char returns float; multi-char
        // returns the same vector dimension.
        size_t n = m.size();
        if (n == 0) return newTypeVar();
        // Every character must be a valid swizzle axis.
        auto isAxis = [](char c) {
            return c == 'x' || c == 'y' || c == 'z' || c == 'w' ||
                   c == 'r' || c == 'g' || c == 'b' || c == 'a';
        };
        bool allAxes = true;
        for (char c : m) {
            if (!isAxis(c)) { allAxes = false; break; }
        }
        if (!allAxes) {
            // Not a swizzle — leave as a fresh var so the analyzer can
            // report a struct-field lookup error later if needed.
            return newTypeVar();
        }
        if (n == 1) {
            // Scalar swizzle: result is a primitive of the same element type.
            switch (vec->elementType()) {
                case PrimitiveType::Float: return BuiltinTypes::Float;
                case PrimitiveType::Int:   return BuiltinTypes::Int;
                // Phase 3.3 Block 3: uvec3.x is `uint` (matches GLSL);
                // before this branch was added it fell through to Dynamic.
                case PrimitiveType::Uint:  return BuiltinTypes::Uint;
                case PrimitiveType::Bool:  return BuiltinTypes::Bool;
                default: return BuiltinTypes::Dynamic;
            }
        }
        // Multi-component swizzle — result is a vector of the requested
        // dimension with the same element type. We currently require the
        // swizzle length to match an existing vector dimension (vec2/3/4).
        //
        // Phase 3.3 Block 3: respect the source element type so
        // `uvec3.xy` resolves to uvec2 (not vec2). Previous code always
        // returned the float-vector singleton; with strict uvec3 typing
        // in place that's a type error.
        switch (vec->elementType()) {
            case PrimitiveType::Int: {
                switch (n) {
                    case 2: return std::make_shared<VectorType>(PrimitiveType::Int, 2);
                    case 3: return std::make_shared<VectorType>(PrimitiveType::Int, 3);
                    case 4: return std::make_shared<VectorType>(PrimitiveType::Int, 4);
                    default: return newTypeVar();
                }
            }
            case PrimitiveType::Uint: {
                switch (n) {
                    case 2: return std::make_shared<VectorType>(PrimitiveType::Uint, 2);
                    case 3: return std::make_shared<VectorType>(PrimitiveType::Uint, 3);
                    case 4: return std::make_shared<VectorType>(PrimitiveType::Uint, 4);
                    default: return newTypeVar();
                }
            }
            case PrimitiveType::Float:
            default: {
                switch (n) {
                    case 2: return BuiltinTypes::Vec2();
                    case 3: return BuiltinTypes::Vec3();
                    case 4: return BuiltinTypes::Vec4();
                    default: return newTypeVar();
                }
            }
        }
    }

    // Member access on a struct (e.g. UniformBlock instance): Lights.dirs
    if (auto st = std::dynamic_pointer_cast<StructType>(concrete)) {
        for (const auto& field : st->fields()) {
            if (field.first == expr.member) {
                return field.second;
            }
        }
        return newTypeVar();
    }

    // Member access on non-vector (e.g. unresolved) — defer to analyzer.
    return newTypeVar();
}

std::shared_ptr<Type> TypeInference::inferIndexExpr(const IndexExpr& expr) {
    auto objectType = infer(*expr.object);
    infer(*expr.index);  // index is i32 — type-checked but not used for result

    // Resolve any pending type variable.
    std::shared_ptr<Type> concrete = objectType;
    while (auto tv = std::dynamic_pointer_cast<TypeVar>(concrete)) {
        if (tv->hasSolution()) {
            concrete = tv->getSolution();
        } else {
            break;
        }
    }

    // Vector[i] → element type (scalar). Arrays would return element type.
    if (auto vec = std::dynamic_pointer_cast<VectorType>(concrete)) {
        switch (vec->elementType()) {
            case PrimitiveType::Float: return BuiltinTypes::Float;
            case PrimitiveType::Int:   return BuiltinTypes::Int;
            // Phase 3.3 Block 3: uvec3[0] is `uint`.
            case PrimitiveType::Uint:  return BuiltinTypes::Uint;
            case PrimitiveType::Bool:  return BuiltinTypes::Bool;
            default: return BuiltinTypes::Dynamic;
        }
    }
    if (auto arr = std::dynamic_pointer_cast<ArrayType>(concrete)) {
        return arr->elementType();
    }
    return newTypeVar();
}

bool TypeInference::unify(std::shared_ptr<Type> a, std::shared_ptr<Type> b) {
    // Phase 2 closing fix: resolve both sides through their TypeVar
    // chains before deciding what to do. The previous code only
    // walked ONE step per call (`tv->getSolution()` directly),
    // which is fine for shallow chains but blows the stack on long
    // arithmetic chains (5+ multiplications build a chain of 5
    // TypeVars each pointing to the next). resolveTypeVar flattens
    // the chain in one shot.
    a = resolveTypeVar(a);
    b = resolveTypeVar(b);
    // Same-instance short-circuit (a unify a is trivially true).
    if (a == b) return true;

    if (auto tvA = std::dynamic_pointer_cast<TypeVar>(a)) {
        if (auto tvB = std::dynamic_pointer_cast<TypeVar>(b)) {
            // Both still TypeVars — bind A to B.
            tvA->setSolution(b);
            return true;
        }
        // a is a type variable, b is a concrete type — bind a to b
        tvA->setSolution(b);
        return true;
    }

    if (auto tvB = std::dynamic_pointer_cast<TypeVar>(b)) {
        // b is a type variable (a is concrete), bind b to a
        tvB->setSolution(a);
        return true;
    }

    // Both are concrete types - check equality
    return a->equals(*b);
}

void ConstraintSolver::addConstraint(std::shared_ptr<Type> left, std::shared_ptr<Type> right, const std::string& location) {
    _constraints.push_back({left, right, location});
}

bool ConstraintSolver::solve() {
    TypeInference inference(*(TypeEnvironment*)nullptr);
    for (const auto& c : _constraints) {
        if (!inference.unify(c.left, c.right)) {
            return false;
        }
    }
    return true;
}

} // namespace ayt::shader::phoskia
