// AYSemanticAnalyzer.cpp - Semantic analysis implementation

#include "AYSemanticAnalyzer.h"
#include "AYBuiltinFunctions.h"
#include <iostream>

namespace ayt::shader::phoskia
{

AYSemanticAnalyzer::AYSemanticAnalyzer(TypeEnvironment& env) : _env(env) {}

bool AYSemanticAnalyzer::analyze(const Program& program) {
    _env.pushScope();

    // Register built-in functions
    for (const auto& name : BuiltinFunctionRegistry::instance().getAllFunctionNames()) {
        auto func = BuiltinFunctionRegistry::instance().getFunction(name);
        if (func) {
            _env.addFunction(name, std::make_shared<FunctionType>(func->paramTypes, func->returnType));
        }
    }

    for (const auto& stmt : program.declarations) {
        analyze(*stmt);
    }

    _env.popScope();
    return !hasErrors();
}

void AYSemanticAnalyzer::analyze(const Stmt& stmt) {
    if (auto material = dynamic_cast<const MaterialDecl*>(&stmt)) {
        analyzeMaterialDecl(*material);
    } else if (auto property = dynamic_cast<const PropertyDecl*>(&stmt)) {
        analyzePropertyDecl(*property);
    } else if (auto uniform = dynamic_cast<const UniformDecl*>(&stmt)) {
        analyzeUniformDecl(*uniform);
    } else if (auto texture = dynamic_cast<const TextureDecl*>(&stmt)) {
        analyzeTextureDecl(*texture);
    } else if (auto vert = dynamic_cast<const VertexFunc*>(&stmt)) {
        analyzeVertexFunc(*vert);
    } else if (auto frag = dynamic_cast<const FragmentFunc*>(&stmt)) {
        analyzeFragmentFunc(*frag);
    } else if (auto let = dynamic_cast<const LetStmt*>(&stmt)) {
        analyzeLetStmt(*let);
    } else if (auto ret = dynamic_cast<const ReturnStmt*>(&stmt)) {
        analyzeReturnStmt(*ret);
    } else if (auto ifstmt = dynamic_cast<const IfStmt*>(&stmt)) {
        analyzeIfStmt(*ifstmt);
    } else if (auto forstmt = dynamic_cast<const ForStmt*>(&stmt)) {
        analyzeForStmt(*forstmt);
    } else if (auto exprstmt = dynamic_cast<const ExprStmt*>(&stmt)) {
        analyzeExpr(*exprstmt->expr);
    }
}

void AYSemanticAnalyzer::analyzeMaterialDecl(const MaterialDecl& decl) {
    _env.pushScope();
    for (const auto& item : decl.declarations) {
        analyze(*item);
    }
    _env.popScope();
}

void AYSemanticAnalyzer::analyzePropertyDecl(const PropertyDecl& decl) {
    auto type = analyzeExpr(*decl.initializer);
    _env.addVariable(decl.name, type);
    _symbols[decl.name] = type;
    _materialProperties[decl.name] = type;
}

void AYSemanticAnalyzer::analyzeUniformDecl(const UniformDecl& decl) {
    // Uniform type inference from name
    std::shared_ptr<Type> type = BuiltinTypes::Dynamic;
    _env.addVariable(decl.name, type);
    _symbols[decl.name] = type;
}

void AYSemanticAnalyzer::analyzeTextureDecl(const TextureDecl& decl) {
    _env.addVariable(decl.name, BuiltinTypes::Dynamic);
    _symbols[decl.name] = BuiltinTypes::Dynamic;
}

void AYSemanticAnalyzer::analyzeShaderParam(const ShaderParam& param) {
    // Phase 1: register the param name in the local scope so the body can
    // reference it. We treat the type as Dynamic (no type inference yet).
    (void)param;
    // _env.addVariable(param.name, BuiltinTypes::Dynamic);  // TODO Phase 2
}

void AYSemanticAnalyzer::analyzeVertexFunc(const VertexFunc& func) {
    _env.pushScope();
    _inShaderFunc = true;
    for (const auto& p : func.params) {
        if (auto sp = dynamic_cast<const ShaderParam*>(p.get())) {
            analyzeShaderParam(*sp);
        }
    }
    for (const auto& stmt : func.body) {
        analyze(*stmt);
    }
    _inShaderFunc = false;
    _env.popScope();
}

void AYSemanticAnalyzer::analyzeFragmentFunc(const FragmentFunc& func) {
    _env.pushScope();
    _inShaderFunc = true;
    for (const auto& p : func.inputs) {
        if (auto sp = dynamic_cast<const ShaderParam*>(p.get())) {
            analyzeShaderParam(*sp);
        }
    }
    for (const auto& stmt : func.body) {
        analyze(*stmt);
    }
    _inShaderFunc = false;
    _env.popScope();
}

void AYSemanticAnalyzer::analyzeLetStmt(const LetStmt& stmt) {
    auto type = analyzeExpr(*stmt.initializer);
    _env.addVariable(stmt.name, type);
    _symbols[stmt.name] = type;
}

void AYSemanticAnalyzer::analyzeReturnStmt(const ReturnStmt& stmt) {
    if (!_inShaderFunc) {
        error("Return statement outside of shader block", 0, 0);
    }
    if (stmt.value) {
        analyzeExpr(*stmt.value);
    }
}

void AYSemanticAnalyzer::analyzeIfStmt(const IfStmt& stmt) {
    auto condType = analyzeExpr(*stmt.condition);
    if (!condType->equals(*BuiltinTypes::Bool)) {
        // Type mismatch warning
    }
    _env.pushScope();
    for (const auto& s : stmt.thenBranch) analyze(*s);
    _env.popScope();
    _env.pushScope();
    for (const auto& s : stmt.elseBranch) analyze(*s);
    _env.popScope();
}

void AYSemanticAnalyzer::analyzeForStmt(const ForStmt& stmt) {
    _env.pushScope();
    auto iterType = analyzeExpr(*stmt.iterable);
    _env.addVariable(stmt.variable, BuiltinTypes::Dynamic);
    for (const auto& s : stmt.body) analyze(*s);
    _env.popScope();
}

std::shared_ptr<Type> AYSemanticAnalyzer::analyzeExpr(const Expr& expr) {
    if (auto binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        auto left = analyzeExpr(*binary->left);
        auto right = analyzeExpr(*binary->right);
        return left;  // Simplified
    }
    if (auto unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        return analyzeExpr(*unary->operand);
    }
    if (auto call = dynamic_cast<const CallExpr*>(&expr)) {
        for (const auto& arg : call->args) {
            analyzeExpr(*arg);
        }
        return BuiltinTypes::Dynamic;
    }
    if (auto id = dynamic_cast<const IdentifierExpr*>(&expr)) {
        auto type = _env.getVariable(id->name);
        if (!type) {
            error("Undefined identifier: " + id->name, 0, 0);
            return BuiltinTypes::Dynamic;
        }
        return type;
    }
    if (auto lit = dynamic_cast<const LiteralExpr*>(&expr)) {
        return std::visit([](auto&& arg) -> std::shared_ptr<Type> {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, float>) return BuiltinTypes::Float;
            if constexpr (std::is_same_v<T, int>) return BuiltinTypes::Int;
            if constexpr (std::is_same_v<T, bool>) return BuiltinTypes::Bool;
            if constexpr (std::is_same_v<T, std::string>) return BuiltinTypes::String;
            return BuiltinTypes::Dynamic;
        }, lit->value);
    }
    if (auto member = dynamic_cast<const MemberExpr*>(&expr)) {
        analyzeExpr(*member->object);
        return BuiltinTypes::Dynamic;
    }
    if (auto index = dynamic_cast<const IndexExpr*>(&expr)) {
        analyzeExpr(*index->object);
        analyzeExpr(*index->index);
        return BuiltinTypes::Dynamic;
    }
    return BuiltinTypes::Dynamic;
}

void AYSemanticAnalyzer::error(const std::string& message, int line, int column) {
    // DEBUG: retained — surfaces the semantic-error reporting path that the
    // Phase 1 F-group fix depends on (see AYPhoskia::runPipeline).
    std::cerr << "[SemanticAnalyzer] error at line=" << line
              << " col=" << column << ": " << message << "\n";
    _reporter.error(ErrorCode::UnknownIdentifier, message, line, column);
}

void AYSemanticAnalyzer::warning(const std::string& message, int line, int column) {
    // Phase 1: warnings are not surfaced via CompilerError. Stored as note-like message.
    // TODO: introduce a separate warning sink or extend CompilerError with level.
    (void)message; (void)line; (void)column;
}

bool AYSemanticAnalyzer::isDefined(const std::string& name) const {
    return _symbols.find(name) != _symbols.end();
}

std::shared_ptr<Type> AYSemanticAnalyzer::getType(const std::string& name) const {
    auto it = _symbols.find(name);
    return (it != _symbols.end()) ? it->second : nullptr;
}

} // namespace ayt::shader::phoskia
