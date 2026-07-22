#pragma once
// AYBuiltinFunctions.h - Extensible built-in function library for Phoskia

#include "AYType.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>

namespace ayt::shader::phoskia
{

// Built-in function signature
struct BuiltinFunction {
    std::string name;
    std::vector<std::shared_ptr<Type>> paramTypes;
    std::shared_ptr<Type> returnType;
    std::string description;

    // Optional implementation for interpreted execution
    std::function<std::variant<std::monostate, bool, float, int, std::string>(const std::vector<std::variant<std::monostate, bool, float, int, std::string>>&)> implementation;
};

// Registry of built-in functions
class BuiltinFunctionRegistry {
public:
    static BuiltinFunctionRegistry& instance();

    void registerFunction(const BuiltinFunction& func);
    bool hasFunction(const std::string& name) const;

    // Look up a function by name only — returns the FIRST registered
    // overload. Useful for "is this a known name?" checks; not safe for
    // type inference because overloads may differ in arity / param types.
    const BuiltinFunction* getFunction(const std::string& name) const;

    // Overload-aware lookup: returns the FIRST overload whose arity matches
    // `argsSize`. Insufficient when multiple same-arity overloads exist
    // (e.g. mix(float,float,float) vs mix(vec3,vec3,float)) — prefer
    // getOverloads + type scoring in the type checker.
    const BuiltinFunction* getFunctionByArity(const std::string& name,
                                              size_t argsSize) const;

    // All registered overloads for `name`, or nullptr if unknown.
    const std::vector<BuiltinFunction>* getOverloads(const std::string& name) const;

    std::vector<std::string> getAllFunctionNames() const;

    // Convenience registration
    template<typename F>
    void registerFunction(const std::string& name, const std::vector<std::shared_ptr<Type>>& params,
                         std::shared_ptr<Type> returnType, F&& impl, const std::string& desc = "") {
        BuiltinFunction func;
        func.name = name;
        func.paramTypes = params;
        func.returnType = returnType;
        func.description = desc;
        func.implementation = impl;
        registerFunction(func);
    }

private:
    BuiltinFunctionRegistry() { registerDefaults(); }
    void registerDefaults();

    std::unordered_map<std::string, std::vector<BuiltinFunction>> _functions;
};

// Math functions
namespace MathFunctions {
    // unary math
    void registerAbs();
    void registerFloor();
    void registerCeil();
    void registerRound();
    void registerSqrt();
    void registerPow();
    void registerLog();
    void registerExp();
    void registerSin();
    void registerCos();
    void registerTan();
    void registerAsin();
    void registerAcos();
    void registerAtan();

    // binary math
    void registerMin();
    void registerMax();
    void registerClamp();
    void registerLerp();
    void registerStep();
    void registerSmoothstep();

    // vector math
    void registerNormalize();
    void registerDot();
    void registerCross();
    void registerLength();
    void registerDistance();
    void registerReflect();
    void registerRefract();
}

// PBR functions
namespace PBRFunctions {
    void registerFresnelSchlick();
    void registerGGXDistribution();
    void registerSmithGGXGeometry();
    void registerCookTorrance();
}

// Texture functions
namespace TextureFunctions {
    void registerSample();
    void registerSampleLod();
    void registerSampleGrad();
}

// Utility functions
namespace UtilityFunctions {
    void registerMix();
    void registerMap();
    void registerReduce();
    void registerFilter();
    void registerIf();
}

// Phase 1 RD-03: skeletal animation helper builtins.
// `skinningMatrix(ivec4 indices, vec4 weights, mat4[] bones, vec4 pos)`
// returns the linear-blend skinning transform applied to `pos`.
// In Phoskia the bones argument is a single `mat4` field (not a
// collection) — the GLSL emit expands the call into a 4-bone
// weighted sum using the field name as the array identifier.
namespace SkeletalFunctions {
    void registerSkinningMatrix();
}

} // namespace ayt::shader::phoskia