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

    // Check if it's a known built-in function
    if (auto id = dynamic_cast<const IdentifierExpr*>(expr.callee.get())) {
        auto builtin = BuiltinFunctionRegistry::instance().getFunction(id->name);
        if (builtin) {
            if (expr.args.size() == builtin->paramTypes.size()) {
                for (size_t i = 0; i < expr.args.size(); ++i) {
                    auto argType = infer(*expr.args[i]);
                    unify(argType, builtin->paramTypes[i]);
                }
                return builtin->returnType;
            }
        }
    }

    // General function call
    if (auto func = dynamic_cast<FunctionType*>(funcType.get())) {
        if (expr.args.size() == func->params().size()) {
            for (size_t i = 0; i < expr.args.size(); ++i) {
                auto argType = infer(*expr.args[i]);
                unify(argType, func->params()[i]);
            }
            return func->returnType();
        }
    }

    return newTypeVar();
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
    infer(*expr.object);
    return newTypeVar();  // Return type depends on member
}

std::shared_ptr<Type> TypeInference::inferIndexExpr(const IndexExpr& expr) {
    infer(*expr.object);
    infer(*expr.index);
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
