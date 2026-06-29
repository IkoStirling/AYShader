// AYType.cpp - Type system implementation

#include "AYType.h"
#include <sstream>

namespace ayt::shader::phoskia
{

std::string PrimitiveType_::toString() const {
    switch (_primitive) {
        case PrimitiveType::Bool: return "bool";
        case PrimitiveType::Int: return "int";
        case PrimitiveType::Uint: return "uint";  // Phase 3.3 Block 1
        case PrimitiveType::Float: return "float";
        case PrimitiveType::String: return "string";
        default: return "unknown";
    }
}

bool PrimitiveType_::equals(const Type& other) const {
    if (auto p = dynamic_cast<const PrimitiveType_*>(&other)) {
        return _primitive == p->_primitive;
    }
    return false;
}

bool VectorType::equals(const Type& other) const {
    if (auto v = dynamic_cast<const VectorType*>(&other)) {
        return _elementType == v->_elementType && _dimension == v->_dimension;
    }
    return false;
}

bool MatrixType::equals(const Type& other) const {
    if (auto m = dynamic_cast<const MatrixType*>(&other)) {
        return _rows == m->_rows && _cols == m->_cols;
    }
    return false;
}

bool ArrayType::equals(const Type& other) const {
    if (auto a = dynamic_cast<const ArrayType*>(&other)) {
        return _elementType->equals(*a->_elementType) && _size == a->_size;
    }
    return false;
}

bool FunctionType::equals(const Type& other) const {
    if (auto f = dynamic_cast<const FunctionType*>(&other)) {
        if (_params.size() != f->_params.size()) return false;
        for (size_t i = 0; i < _params.size(); ++i) {
            if (!_params[i]->equals(*f->_params[i])) return false;
        }
        return _returnType->equals(*f->_returnType);
    }
    return false;
}

bool StructType::equals(const Type& other) const {
    if (auto s = dynamic_cast<const StructType*>(&other)) {
        return _name == s->_name;
    }
    return false;
}

void TypeEnvironment::addVariable(const std::string& name, std::shared_ptr<Type> type) {
    if (_scopes.empty()) pushScope();
    _scopes.back()[name] = type;
}

std::shared_ptr<Type> TypeEnvironment::getVariable(const std::string& name) const {
    for (auto it = _scopes.rbegin(); it != _scopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return found->second;
    }
    return nullptr;
}

bool TypeEnvironment::hasVariable(const std::string& name) const {
    return getVariable(name) != nullptr;
}

void TypeEnvironment::addFunction(const std::string& name, std::shared_ptr<FunctionType> type) {
    _functions[name] = type;
}

std::shared_ptr<FunctionType> TypeEnvironment::getFunction(const std::string& name) const {
    auto it = _functions.find(name);
    return (it != _functions.end()) ? it->second : nullptr;
}

void TypeEnvironment::pushScope() {
    _scopes.emplace_back();
}

void TypeEnvironment::popScope() {
    if (!_scopes.empty()) _scopes.pop_back();
}

// Built-in type instances
namespace BuiltinTypes {
    std::shared_ptr<PrimitiveType_> Bool = std::make_shared<PrimitiveType_>(PrimitiveType::Bool);
    std::shared_ptr<PrimitiveType_> Int = std::make_shared<PrimitiveType_>(PrimitiveType::Int);
    // Phase 3.3 Block 1: GLSL `uint` lexeme → PrimitiveType::Uint
    // singleton. Used by storage buffer element types (uint counters)
    // and any future Phase 3.3 strict-uvec3 plumbing (uvec3's element
    // type is Uint). Vector forms (uvec2..4) are Phase 3.3-Block 3.
    std::shared_ptr<PrimitiveType_> Uint = std::make_shared<PrimitiveType_>(PrimitiveType::Uint);
    std::shared_ptr<PrimitiveType_> Float = std::make_shared<PrimitiveType_>(PrimitiveType::Float);
    std::shared_ptr<PrimitiveType_> String = std::make_shared<PrimitiveType_>(PrimitiveType::String);
    std::shared_ptr<Type> Void = nullptr;  // Will be set below
    // Dynamic is a placeholder for unknown types; uses DynamicType subclass.
    // (Cannot instantiate Type directly because it has pure virtuals.)
    std::shared_ptr<Type> Dynamic = std::make_shared<DynamicType>();

    std::shared_ptr<VectorType> Vec2() { return std::make_shared<VectorType>(PrimitiveType::Float, 2); }
    std::shared_ptr<VectorType> Vec3() { return std::make_shared<VectorType>(PrimitiveType::Float, 3); }
    std::shared_ptr<VectorType> Vec4() { return std::make_shared<VectorType>(PrimitiveType::Float, 4); }

    std::shared_ptr<MatrixType> Mat2() { return std::make_shared<MatrixType>(2, 2); }
    std::shared_ptr<MatrixType> Mat3() { return std::make_shared<MatrixType>(3, 3); }
    std::shared_ptr<MatrixType> Mat4() { return std::make_shared<MatrixType>(4, 4); }
} // namespace BuiltinTypes

} // namespace ayt::shader::phoskia
