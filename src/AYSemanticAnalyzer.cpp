// AYSemanticAnalyzer.cpp - Semantic analysis implementation

#include "AYSemanticAnalyzer.h"
#include "AYTypeInference.h"  // Phase 2 Step 2: TypeInference + TypeVar for
                               // analyzePropertyDecl / analyzeExpr delegation.
#include "AYBuiltinTypes.h"   // Phase 2 Step 5: isBuiltinType gate on
                               // uniform / property type lexemes.
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
    } else if (auto ub = dynamic_cast<const UniformBlockDecl*>(&stmt)) {
        analyzeUniformBlockDecl(*ub);
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
    // Phase 2 Step 2: run the real type-inference engine on the initializer
    // so `property color = vec4(1.0, ...)` records a VectorType(4) entry
    // and `property tint = 1.0` records a Float. The engine handles
    // constructor calls and member access — see AYTypeInference.h.
    TypeInference inference(_env);
    auto type = inference.infer(*decl.initializer);
    _env.addVariable(decl.name, type);
    _symbols[decl.name] = type;
    _materialProperties[decl.name] = type;
}

void AYSemanticAnalyzer::analyzeUniformDecl(const UniformDecl& decl) {
    // Phase 2 Step 5: validate `decl.type` (the lexeme captured by the
    // parser when it consumed the uniform's type-name token) against the
    // builtin type table. After the Step 5 token demotion, builtin type
    // names are plain Identifier tokens — the parser accepts ANY
    // identifier here, so a stray name like `uniform hello x;` would
    // otherwise silently pass the parser and only blow up at the BGFX
    // backend. We intercept it here with a Go-style diagnostic:
    //
    //   line N: 'hello' is not a builtin type (expected: float, vec2, ...)
    if (!AYBuiltinTypes::isBuiltinType(decl.type)) {
        error("line " + std::to_string(0) + ": '" + decl.type +
              "' is not a builtin type (expected: " +
              AYBuiltinTypes::expectedList() + ")",
              0, 0);
    }
    // Even on the error path we still register the uniform so later
    // analysis of the body doesn't crash on the dangling identifier.
    // The body might reference the uniform and we don't want to drown
    // the user in "undefined identifier" follow-up errors on top of
    // the type-mismatch error.
    std::shared_ptr<Type> type = BuiltinTypes::Dynamic;
    _env.addVariable(decl.name, type);
    _symbols[decl.name] = type;
}

void AYSemanticAnalyzer::analyzeTextureDecl(const TextureDecl& decl) {
    // Phase 2 Step 2: textures have a dedicated semantic — we keep the
    // existing Dynamic placeholder so `sample(tex, uv)` still resolves
    // (the sample builtin takes a Texture2D param which currently maps
    // to Dynamic in the env).
    _env.addVariable(decl.name, BuiltinTypes::Dynamic);
    _symbols[decl.name] = BuiltinTypes::Dynamic;
}

void AYSemanticAnalyzer::analyzeUniformBlockDecl(const UniformBlockDecl& decl) {
    // Phase 4-N: register UBO names so `Camera.position` member access
    // resolves during semantic analysis (layout/binding validation stays
    // in the BGFX converter).
    _env.addVariable(decl.name, BuiltinTypes::Dynamic);
    _symbols[decl.name] = BuiltinTypes::Dynamic;
}

void AYSemanticAnalyzer::analyzeShaderParam(const ShaderParam& param) {
    // Phase 2 Step 2: register the param name with its Phoskia semantic
    // type so the body can reference it. The semantic → GLSL-type mapping
    // is fixed by the Phoskia spec (§6.2 in design.md):
    //
    //   position → vec3   (vertex world-space position)
    //   normal   → vec3   (normal vector)
    //   color    → vec4   (vertex/fragment color)
    //   texcoord → vec2   (UV coordinates)
    //
    // The `out` direction may carry a defaultValue; we don't yet use it
    // here (the converter still owns default-value text), but we make
    // sure the param is visible in the body regardless.
    std::shared_ptr<Type> type = BuiltinTypes::Dynamic;
    switch (param.semantic) {
        case PhoskiaSemantic::Position: type = BuiltinTypes::Vec3(); break;
        case PhoskiaSemantic::Normal:   type = BuiltinTypes::Vec3(); break;
        case PhoskiaSemantic::Color:    type = BuiltinTypes::Vec4(); break;
        case PhoskiaSemantic::Texcoord: type = BuiltinTypes::Vec2(); break;
    }
    _env.addVariable(param.name, type);
    _symbols[param.name] = type;
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
        return;
    }
    if (!stmt.value) return;
    auto valType = analyzeExpr(*stmt.value);

    // Resolve any TypeVar wrapper so we can compare against the canonical
    // vec4 expected at the block exit (vertex → gl_Position, fragment →
    // gl_FragColor — both are vec4).
    std::shared_ptr<Type> concrete = valType;
    while (auto tv = std::dynamic_pointer_cast<TypeVar>(concrete)) {
        if (tv->hasSolution()) concrete = tv->getSolution();
        else break;
    }

    auto vec4 = BuiltinTypes::Vec4();
    if (concrete && !concrete->equals(*vec4)) {
        // Allow Dynamic (incomplete inference) and TypeVar (unresolved) —
        // those should not trigger a false-positive error.
        auto dyn = BuiltinTypes::Dynamic;
        bool isUnresolved =
            std::dynamic_pointer_cast<TypeVar>(concrete) != nullptr ||
            (dyn && concrete->equals(*dyn));
        if (!isUnresolved) {
            error("Return type must be vec4 (vertex outputs gl_Position, "
                  "fragment outputs gl_FragColor); got " + concrete->toString(),
                  0, 0);
        }
    }
}

void AYSemanticAnalyzer::analyzeIfStmt(const IfStmt& stmt) {
    auto condType = analyzeExpr(*stmt.condition);

    // Resolve any TypeVar wrapper.
    std::shared_ptr<Type> concrete = condType;
    while (auto tv = std::dynamic_pointer_cast<TypeVar>(concrete)) {
        if (tv->hasSolution()) concrete = tv->getSolution();
        else break;
    }

    auto boolType = BuiltinTypes::Bool;
    auto dyn = BuiltinTypes::Dynamic;
    bool isBoolOrUnresolved =
        (concrete && boolType && concrete->equals(*boolType)) ||
        std::dynamic_pointer_cast<TypeVar>(concrete) != nullptr ||
        (dyn && concrete && concrete->equals(*dyn));
    if (!isBoolOrUnresolved) {
        error("If condition must be bool; got " + concrete->toString(), 0, 0);
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

    // Resolve any TypeVar wrapper.
    std::shared_ptr<Type> concrete = iterType;
    while (auto tv = std::dynamic_pointer_cast<TypeVar>(concrete)) {
        if (tv->hasSolution()) concrete = tv->getSolution();
        else break;
    }

    // The loop variable's type is the element type of the iterable.
    // Vectors and arrays contribute their element type; scalar iterables
    // contribute themselves; Dynamic / unresolved pass through Dynamic.
    std::shared_ptr<Type> elemType = BuiltinTypes::Dynamic;
    if (auto vec = std::dynamic_pointer_cast<VectorType>(concrete)) {
        switch (vec->elementType()) {
            case PrimitiveType::Float: elemType = BuiltinTypes::Float; break;
            case PrimitiveType::Int:   elemType = BuiltinTypes::Int;   break;
            case PrimitiveType::Bool:  elemType = BuiltinTypes::Bool;  break;
            default: break;
        }
    } else if (auto arr = std::dynamic_pointer_cast<ArrayType>(concrete)) {
        elemType = arr->elementType();
    } else if (concrete && !std::dynamic_pointer_cast<TypeVar>(concrete)) {
        // Iterable is a concrete scalar — loop var is that scalar.
        elemType = concrete;
    }
    _env.addVariable(stmt.variable, elemType);
    _symbols[stmt.variable] = elemType;

    for (const auto& s : stmt.body) analyze(*s);
    _env.popScope();
}

std::shared_ptr<Type> AYSemanticAnalyzer::analyzeExpr(const Expr& expr) {
    // Phase 2 Step 2 Step 2-fix: walk the expression tree first to
    // collect every leaf identifier. Each must be defined in the env
    // (variable or function). Skipping MemberExpr.object's nested
    // identifier — `.x` / `.rgb` etc. are swizzle axes, not user
    // names — and IndexExpr.index's nested identifier (when the index
    // is `i` of `for (i in items)` the loop var is in scope, but the
    // TypeInference engine already resolved it during the binary
    // unify; we still want to surface it here for completeness).
    //
    // MemberExpr.member and MemberExpr swizzle axes are NOT identifiers
    // (they're already strings), so we skip them.
    std::vector<const IdentifierExpr*> idents;
    collectIdentifiers(expr, idents);
    for (const auto* id : idents) {
        if (!_env.getVariable(id->name) && !_env.getFunction(id->name)) {
            // Builtin names like `vec3`, `normalize`, `sample` are
            // registered as functions by analyze(const Program&) at
            // the top of the run; if we still can't find the name, it's
            // genuinely undefined.
            if (!BuiltinFunctionRegistry::instance().hasFunction(id->name)) {
                error("Undefined identifier: " + id->name, 0, 0);
            }
        }
    }

    // Delegate to the type-inference engine for the real type. The
    // engine unifies type variables against builtin signatures / env
    // entries and resolves swizzles, array indices, and constructors.
    TypeInference inference(_env);
    return inference.infer(expr);
}

void AYSemanticAnalyzer::collectIdentifiers(const Expr& expr,
                                            std::vector<const IdentifierExpr*>& out) {
    if (auto id = dynamic_cast<const IdentifierExpr*>(&expr)) {
        out.push_back(id);
        return;
    }
    if (auto bin = dynamic_cast<const BinaryExpr*>(&expr)) {
        if (bin->left) collectIdentifiers(*bin->left, out);
        if (bin->right) collectIdentifiers(*bin->right, out);
        return;
    }
    if (auto un = dynamic_cast<const UnaryExpr*>(&expr)) {
        if (un->operand) collectIdentifiers(*un->operand, out);
        return;
    }
    if (auto call = dynamic_cast<const CallExpr*>(&expr)) {
        // Callee is typically an IdentifierExpr — collect it. Nested
        // args are walked below.
        if (call->callee) collectIdentifiers(*call->callee, out);
        for (const auto& a : call->args) collectIdentifiers(*a, out);
        return;
    }
    if (auto mem = dynamic_cast<const MemberExpr*>(&expr)) {
        // Only the object is a candidate identifier — the member name
        // is a swizzle/field string, not a user name.
        if (mem->object) collectIdentifiers(*mem->object, out);
        return;
    }
    if (auto ix = dynamic_cast<const IndexExpr*>(&expr)) {
        if (ix->object) collectIdentifiers(*ix->object, out);
        if (ix->index) collectIdentifiers(*ix->index, out);
        return;
    }
    // LiteralExpr: nothing to collect.
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
