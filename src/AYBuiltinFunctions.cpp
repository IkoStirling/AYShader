// AYBuiltinFunctions.cpp - Built-in function library implementation

#include "AYBuiltinFunctions.h"
#include <cmath>
#include <algorithm>

namespace ayt::shader::phoskia
{

BuiltinFunctionRegistry& BuiltinFunctionRegistry::instance() {
    static BuiltinFunctionRegistry registry;
    return registry;
}

void BuiltinFunctionRegistry::registerFunction(const BuiltinFunction& func) {
    _functions[func.name] = func;
}

bool BuiltinFunctionRegistry::hasFunction(const std::string& name) const {
    return _functions.find(name) != _functions.end();
}

const BuiltinFunction* BuiltinFunctionRegistry::getFunction(const std::string& name) const {
    auto it = _functions.find(name);
    return (it != _functions.end()) ? &it->second : nullptr;
}

std::vector<std::string> BuiltinFunctionRegistry::getAllFunctionNames() const {
    std::vector<std::string> names;
    for (const auto& pair : _functions) {
        names.push_back(pair.first);
    }
    return names;
}

void BuiltinFunctionRegistry::registerDefaults() {
    using TF = std::shared_ptr<Type>;
    auto F = BuiltinTypes::Float;
    auto I = BuiltinTypes::Int;
    auto B = BuiltinTypes::Bool;
    auto V2 = BuiltinTypes::Vec2();
    auto V3 = BuiltinTypes::Vec3();
    auto V4 = BuiltinTypes::Vec4();

    // Math functions
    auto mathImpl = [](float x) -> float { return std::abs(x); };
    registerFunction("abs", {F}, F, [](auto args) -> float {
        return std::abs(std::get<float>(args[0]));
    }, "Absolute value");

    registerFunction("floor", {F}, F, [](auto args) -> float {
        return std::floor(std::get<float>(args[0]));
    });

    registerFunction("ceil", {F}, F, [](auto args) -> float {
        return std::ceil(std::get<float>(args[0]));
    });

    registerFunction("round", {F}, F, [](auto args) -> float {
        return std::round(std::get<float>(args[0]));
    });

    registerFunction("sqrt", {F}, F, [](auto args) -> float {
        return std::sqrt(std::get<float>(args[0]));
    });

    registerFunction("pow", {F, F}, F, [](auto args) -> float {
        return std::pow(std::get<float>(args[0]), std::get<float>(args[1]));
    });

    registerFunction("exp", {F}, F, [](auto args) -> float {
        return std::exp(std::get<float>(args[0]));
    });

    registerFunction("log", {F}, F, [](auto args) -> float {
        return std::log(std::get<float>(args[0]));
    });

    registerFunction("sin", {F}, F, [](auto args) -> float {
        return std::sin(std::get<float>(args[0]));
    });

    registerFunction("cos", {F}, F, [](auto args) -> float {
        return std::cos(std::get<float>(args[0]));
    });

    registerFunction("tan", {F}, F, [](auto args) -> float {
        return std::tan(std::get<float>(args[0]));
    });

    registerFunction("min", {F, F}, F, [](auto args) -> float {
        return std::min(std::get<float>(args[0]), std::get<float>(args[1]));
    });

    registerFunction("max", {F, F}, F, [](auto args) -> float {
        return std::max(std::get<float>(args[0]), std::get<float>(args[1]));
    });

    registerFunction("clamp", {F, F, F}, F, [](auto args) -> float {
        float v = std::get<float>(args[0]);
        float mn = std::get<float>(args[1]);
        float mx = std::get<float>(args[2]);
        return (v < mn) ? mn : (v > mx) ? mx : v;
    });

    registerFunction("lerp", {F, F, F}, F, [](auto args) -> float {
        float a = std::get<float>(args[0]);
        float b = std::get<float>(args[1]);
        float t = std::get<float>(args[2]);
        return a + t * (b - a);
    });

    registerFunction("step", {F, F}, F, [](auto args) -> float {
        float edge = std::get<float>(args[0]);
        float x = std::get<float>(args[1]);
        return x < edge ? 0.0f : 1.0f;
    });

    registerFunction("smoothstep", {F, F, F}, F, [](auto args) -> float {
        float e0 = std::get<float>(args[0]);
        float e1 = std::get<float>(args[1]);
        float x = std::get<float>(args[2]);
        float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    });

    // Conditional
    registerFunction("if", {B, F, F}, F,
        [](auto args) -> float {
            bool cond = std::get<bool>(args[0]);
            return cond ? std::get<float>(args[1]) : std::get<float>(args[2]);
        }, "Ternary conditional");
}

// Namespace registration functions
namespace MathFunctions {
    void registerAbs() {
        // Already registered in registerDefaults
    }
    void registerFloor() {}
    void registerCeil() {}
    void registerRound() {}
    void registerSqrt() {}
    void registerPow() {}
    void registerLog() {}
    void registerExp() {}
    void registerSin() {}
    void registerCos() {}
    void registerTan() {}
    void registerAsin() {}
    void registerAcos() {}
    void registerAtan() {}
    void registerMin() {}
    void registerMax() {}
    void registerClamp() {}
    void registerLerp() {}
    void registerStep() {}
    void registerSmoothstep() {}
    void registerNormalize() {}
    void registerDot() {}
    void registerCross() {}
    void registerLength() {}
    void registerDistance() {}
    void registerReflect() {}
    void registerRefract() {}
}

namespace PBRFunctions {
    void registerFresnelSchlick() {}
    void registerGGXDistribution() {}
    void registerSmithGGXGeometry() {}
    void registerCookTorrance() {}
}

namespace TextureFunctions {
    void registerSample() {}
    void registerSampleLod() {}
    void registerSampleGrad() {}
}

namespace UtilityFunctions {
    void registerMix() {}
    void registerMap() {}
    void registerReduce() {}
    void registerFilter() {}
    void registerIf() {}
}

} // namespace ayt::shader::phoskia