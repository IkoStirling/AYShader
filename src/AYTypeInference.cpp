// AYTypeInference.cpp - Hindley-Milner type inference implementation

#include "AYTypeInference.h"
#include "AYBuiltinFunctions.h"
#include <stdexcept>

namespace ayt::shader::phoskia
{

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

    // Arithmetic operations require same numeric types
    if (expr.op.type == TokenType::Plus || expr.op.type == TokenType::Minus ||
        expr.op.type == TokenType::Star || expr.op.type == TokenType::Slash) {
        unify(leftType, rightType);
        unify(resultType, leftType);
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
        // Phase 2 Step 2: use arity-aware overload lookup so `normalize(v3)`
        // picks the vec3→vec3 overload and not the (later-registered)
        // vec4→vec4 one. Without this, the last-registered overload wins.
        auto builtin = BuiltinFunctionRegistry::instance().getFunctionByArity(
            id->name, expr.args.size());
        if (builtin) {
            for (size_t i = 0; i < expr.args.size(); ++i) {
                auto argType = infer(*expr.args[i]);
                unify(argType, builtin->paramTypes[i]);
            }
            return builtin->returnType;
        }

        // No overload matched the arity. Try by-name fallback for arity
        // mismatches: still infer each arg for error recovery, return
        // the return type of the first overload so the analyzer can
        // surface a "wrong number of arguments" message.
        auto anyOvl = BuiltinFunctionRegistry::instance().getFunction(id->name);
        if (anyOvl) {
            for (const auto& a : expr.args) infer(*a);
            return anyOvl->returnType;
        }

        // ---- Type constructor fallback (when not registered as builtin) ----
        // Phase 1 registers scalar/vector builtins via registerDefaults;
        // this branch handles arities those don't cover, e.g. vec4(v3, f),
        // mat4(v4, v4, v4, v4).
        const std::string& name = id->name;
        // Total component count inferred from the constructor's argument shape.
        // Rules:
        //   vec2(f, f) | vec2(vec2)                    → 2 components
        //   vec3(f, f, f) | vec3(vec3) | vec3(vec2, f) → 3 components
        //   vec4(f, f, f, f) | vec4(vec4) | vec4(vec3, f) | vec4(vec2, f, f)
        //                                                  → 4 components
        //   mat4(v4, v4, v4, v4)                        → mat4
        if (name == "vec2" || name == "vec3" || name == "vec4" ||
            name == "ivec2" || name == "ivec3" || name == "ivec4" ||
            name == "mat2" || name == "mat3" || name == "mat4") {
            auto inferred = inferConstructor(name, expr.args);
            if (inferred) return inferred;
        }
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
        bool isInt = (name.size() > 0 && name[0] == 'i');  // ivec2/3/4
        switch (dim) {
            case 2: return std::make_shared<VectorType>(isInt ? PrimitiveType::Int : PrimitiveType::Float, 2);
            case 3: return std::make_shared<VectorType>(isInt ? PrimitiveType::Int : PrimitiveType::Float, 3);
            case 4: return std::make_shared<VectorType>(isInt ? PrimitiveType::Int : PrimitiveType::Float, 4);
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
    else if (name[0] == 'i') dim = name[4] - '0';   // ivec2/3/4
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
                case PrimitiveType::Bool:  return BuiltinTypes::Bool;
                default: return BuiltinTypes::Dynamic;
            }
        }
        // Multi-component swizzle — result is a vector of the requested
        // dimension with the same element type. We currently require the
        // swizzle length to match an existing vector dimension (vec2/3/4).
        switch (n) {
            case 2: return BuiltinTypes::Vec2();
            case 3: return BuiltinTypes::Vec3();
            case 4: return BuiltinTypes::Vec4();
            default: return newTypeVar();
        }
    }

    // Member access on non-vector (e.g. struct field) — defer to analyzer.
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
    // If both are type variables, bind them
    if (auto tvA = std::dynamic_pointer_cast<TypeVar>(a)) {
        if (auto tvB = std::dynamic_pointer_cast<TypeVar>(b)) {
            if (tvA->hasSolution() && tvB->hasSolution()) {
                return unify(tvA->getSolution(), tvB->getSolution());
            }
            if (tvA->hasSolution()) return unify(tvA->getSolution(), b);
            if (tvB->hasSolution()) return unify(a, tvB->getSolution());
            // Neither has solution - bind A to B
            tvA->setSolution(b);
            return true;
        }
        // a is a type variable, b is a concrete type — bind a to b
        tvA->setSolution(b);
        return true;
    }

    if (auto tvB = std::dynamic_pointer_cast<TypeVar>(b)) {
        if (tvB->hasSolution()) return unify(a, tvB->getSolution());
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
