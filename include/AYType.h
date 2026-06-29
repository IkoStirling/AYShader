#pragma once
// AYType.h - Generic type system for Phoskia

#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <unordered_map>

namespace ayt::shader::phoskia
{

// Type kinds
enum class TypeKind {
    Primitive,
    Vector,
    Matrix,
    Array,
    Tuple,
    Function,
    Struct,
    Custom,
    Void,
    Dynamic
};

// Primitive types
enum class PrimitiveType {
    Bool,
    Int,
    Uint,
    Float,
    String
};

// Type representation
class Type {
public:
    explicit Type(TypeKind kind) : _kind(kind) {}
    virtual ~Type() = default;

    TypeKind kind() const { return _kind; }
    virtual std::string toString() const = 0;
    virtual bool equals(const Type& other) const = 0;

private:
    TypeKind _kind;
};

// Dynamic / unknown type — concrete placeholder.
class DynamicType : public Type {
public:
    DynamicType() : Type(TypeKind::Dynamic) {}
    std::string toString() const override { return "dynamic"; }
    bool equals(const Type& other) const override {
        return other.kind() == TypeKind::Dynamic;
    }
};

// Primitive type
class PrimitiveType_ : public Type {
public:
    explicit PrimitiveType_(PrimitiveType type)
        : Type(TypeKind::Primitive), _primitive(type) {}

    PrimitiveType primitive() const { return _primitive; }
    std::string toString() const override;
    bool equals(const Type& other) const override;

private:
    PrimitiveType _primitive;
};

// Vector type (generic N-dimensional)
class VectorType : public Type {
public:
    VectorType(PrimitiveType elementType, size_t dimension)
        : Type(TypeKind::Vector), _elementType(elementType), _dimension(dimension) {}

    PrimitiveType elementType() const { return _elementType; }
    size_t dimension() const { return _dimension; }

    std::string toString() const override {
        return "vec" + std::to_string(_dimension);
    }
    bool equals(const Type& other) const override;

private:
    PrimitiveType _elementType;
    size_t _dimension;
};

// Matrix type
class MatrixType : public Type {
public:
    MatrixType(size_t rows, size_t cols)
        : Type(TypeKind::Matrix), _rows(rows), _cols(cols) {}

    size_t rows() const { return _rows; }
    size_t cols() const { return _cols; }

    std::string toString() const override {
        return "mat" + std::to_string(_rows) + "x" + std::to_string(_cols);
    }
    bool equals(const Type& other) const override;

private:
    size_t _rows;
    size_t _cols;
};

// Array type
class ArrayType : public Type {
public:
    ArrayType(std::shared_ptr<Type> elementType, size_t size = 0)
        : Type(TypeKind::Array), _elementType(elementType), _size(size) {}

    std::shared_ptr<Type> elementType() const { return _elementType; }
    size_t size() const { return _size; }

    std::string toString() const override {
        if (_size > 0) {
            return "array<" + _elementType->toString() + ", " + std::to_string(_size) + ">";
        }
        return "array<" + _elementType->toString() + ">";
    }
    bool equals(const Type& other) const override;

private:
    std::shared_ptr<Type> _elementType;
    size_t _size;
};

// Function type
class FunctionType : public Type {
public:
    FunctionType(std::vector<std::shared_ptr<Type>> params, std::shared_ptr<Type> returnType)
        : Type(TypeKind::Function), _params(std::move(params)), _returnType(returnType) {}

    const std::vector<std::shared_ptr<Type>>& params() const { return _params; }
    std::shared_ptr<Type> returnType() const { return _returnType; }

    std::string toString() const override {
        std::string s = "(";
        for (size_t i = 0; i < _params.size(); ++i) {
            if (i > 0) s += ", ";
            s += _params[i]->toString();
        }
        s += ") -> " + _returnType->toString();
        return s;
    }
    bool equals(const Type& other) const override;

private:
    std::vector<std::shared_ptr<Type>> _params;
    std::shared_ptr<Type> _returnType;
};

// Struct type
class StructType : public Type {
public:
    StructType(const std::string& name, std::vector<std::pair<std::string, std::shared_ptr<Type>>> fields)
        : Type(TypeKind::Struct), _name(name), _fields(std::move(fields)) {}

    const std::string& name() const { return _name; }
    const std::vector<std::pair<std::string, std::shared_ptr<Type>>>& fields() const { return _fields; }

    std::string toString() const override { return _name; }
    bool equals(const Type& other) const override;

private:
    std::string _name;
    std::vector<std::pair<std::string, std::shared_ptr<Type>>> _fields;
};

// Type environment for type checking/inference
class TypeEnvironment {
public:
    void addVariable(const std::string& name, std::shared_ptr<Type> type);
    std::shared_ptr<Type> getVariable(const std::string& name) const;
    bool hasVariable(const std::string& name) const;

    void addFunction(const std::string& name, std::shared_ptr<FunctionType> type);
    std::shared_ptr<FunctionType> getFunction(const std::string& name) const;

    void pushScope();
    void popScope();

private:
    std::vector<std::unordered_map<std::string, std::shared_ptr<Type>>> _scopes;
    std::unordered_map<std::string, std::shared_ptr<FunctionType>> _functions;
};

// Built-in types (singleton instances)
namespace BuiltinTypes {
    extern std::shared_ptr<PrimitiveType_> Bool;
    extern std::shared_ptr<PrimitiveType_> Int;
    extern std::shared_ptr<PrimitiveType_> Uint;
    extern std::shared_ptr<PrimitiveType_> Float;
    extern std::shared_ptr<PrimitiveType_> String;
    extern std::shared_ptr<Type> Void;
    extern std::shared_ptr<Type> Dynamic;

    std::shared_ptr<VectorType> Vec2();
    std::shared_ptr<VectorType> Vec3();
    std::shared_ptr<VectorType> Vec4();

    std::shared_ptr<MatrixType> Mat2();
    std::shared_ptr<MatrixType> Mat3();
    std::shared_ptr<MatrixType> Mat4();
} // namespace BuiltinTypes

} // namespace ayt::shader::phoskia
