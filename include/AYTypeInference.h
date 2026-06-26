#pragma once
// AYTypeInference.h - Hindley-Milner style type inference for Phoskia

#include "AYType.h"
#include "AYAst.h"
#include <unordered_map>
#include <memory>
#include <vector>
#include <string>

namespace ayt::shader::phoskia
{

// Type variable for inference
class TypeVar : public Type {
public:
    TypeVar(const std::string& name = "")
        : Type(TypeKind::Dynamic), _name(name), _solution(nullptr) {}

    std::string toString() const override {
        if (_solution) return _solution->toString();
        return _name.empty() ? "_" : _name;
    }
    bool equals(const Type& other) const override {
        if (auto tv = dynamic_cast<const TypeVar*>(&other)) {
            return this == tv;
        }
        if (_solution) return _solution->equals(other);
        return false;
    }

    bool hasSolution() const { return _solution != nullptr; }
    std::shared_ptr<Type> getSolution() const { return _solution; }
    void setSolution(std::shared_ptr<Type> type) { _solution = type; }

private:
    std::string _name;
    std::shared_ptr<Type> _solution;
};

class TypeInference {
public:
    explicit TypeInference(TypeEnvironment& env) : _env(env) {}

    // Infer type of an expression
    std::shared_ptr<Type> infer(const Expr& expr);

    // Unify two types
    bool unify(std::shared_ptr<Type> a, std::shared_ptr<Type> b);

    // Get all type variables created during inference
    const std::vector<std::shared_ptr<TypeVar>>& typeVariables() const { return _typeVars; }

private:
    std::shared_ptr<Type> inferBinaryExpr(const BinaryExpr& expr);
    std::shared_ptr<Type> inferUnaryExpr(const UnaryExpr& expr);
    std::shared_ptr<Type> inferCallExpr(const CallExpr& expr);
    std::shared_ptr<Type> inferIdentifierExpr(const IdentifierExpr& expr);
    std::shared_ptr<Type> inferLiteralExpr(const LiteralExpr& expr);
    std::shared_ptr<Type> inferMemberExpr(const MemberExpr& expr);
    std::shared_ptr<Type> inferIndexExpr(const IndexExpr& expr);

    std::shared_ptr<Type> newTypeVar(const std::string& name = "") {
        auto tv = std::make_shared<TypeVar>(name);
        _typeVars.push_back(tv);
        return tv;
    }

    TypeEnvironment& _env;
    std::vector<std::shared_ptr<TypeVar>> _typeVars;
};

// Constraint generation for type checking
struct TypeConstraint {
    std::shared_ptr<Type> left;
    std::shared_ptr<Type> right;
    std::string location;
};

class ConstraintSolver {
public:
    void addConstraint(std::shared_ptr<Type> left, std::shared_ptr<Type> right, const std::string& location);
    bool solve();

private:
    std::vector<TypeConstraint> _constraints;
};

} // namespace ayt::shader::phoskia
