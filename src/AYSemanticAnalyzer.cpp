// AYSemanticAnalyzer.cpp - Semantic analysis implementation

#include "AYShader/SemanticAnalyzer.h"
#include "AYShader/TypeInference.h"  // Phase 2 Step 2: TypeInference + TypeVar for
                               // analyzePropertyDecl / analyzeExpr delegation.
#include "AYShader/BuiltinTypes.h"   // Phase 2 Step 5: isBuiltinType gate on
                               // uniform / property type lexemes.
#include "AYShader/BuiltinFunctions.h"
#include "AYShader/detail/PhoskiaFrameBuiltins.h"
#include <iostream>
#include <unordered_set>

namespace ayt::shader::phoskia
{

namespace {

bool isCompositeTypeConstructor(const std::string& name)
{
    return name == "vec2" || name == "vec3" || name == "vec4"
        || name == "ivec2" || name == "ivec3" || name == "ivec4"
        || name == "uvec2" || name == "uvec3" || name == "uvec4"
        || name == "mat2" || name == "mat3" || name == "mat4";
}

std::shared_ptr<Type> resolveTypeVarChain(std::shared_ptr<Type> type)
{
    std::unordered_set<const Type*> visited;
    while (auto typeVar = std::dynamic_pointer_cast<TypeVar>(type)) {
        if (!typeVar->hasSolution() || !visited.insert(typeVar.get()).second) {
            break;
        }
        auto solution = typeVar->getSolution();
        if (!solution || solution.get() == type.get()) {
            break;
        }
        type = std::move(solution);
    }
    return type;
}

} // namespace

AYSemanticAnalyzer::AYSemanticAnalyzer(TypeEnvironment& env) : _env(env) {}

bool AYSemanticAnalyzer::analyze(const Program& program) {
    // IR-M-08 fix: reset analyzer-wide state at the top of every run so
    // successive analyze() calls (or mid-run exceptions leaving bad state)
    // can't leak scope depth / shader-func flag / MRT counter across runs.
    // The previous code only updated these flags inside the
    // analyzeVertexFunc / analyzeFragmentFunc paths, so a `return` from
    // analyze() while _inShaderFunc was still true would corrupt the
    // next call.
    _inShaderFunc = false;
    _fragmentMrtOutputCount = 0;
    _currentLine = 0;
    _currentColumn = 0;
    _reporter.clear();
    _warnings.clear();

    _env.pushScope();

    // IR-M-08 fix: stash the program pointer so the unused-binding walker
    // (IR-M-04) can re-walk the AST after the main pass completes. Reset
    // state at entry so successive analyze() calls on different Programs
    // don't leak let-bindings / decl records across runs.
    _currentProgram = &program;
    _usedBindings.clear();
    _bindingUseCount.clear();
    _declaredBindings.clear();

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

    // IR-M-04: emit warnings for let-bindings declared but never read.
    // Runs after the main pass so _usedBindings is fully populated.
    emitUnusedBindingWarnings();

    _currentProgram = nullptr;
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
    // constructor calls and member access — see AYShader/TypeInference.h.
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
        // IR-H-01 follow-up: use the current-location hint so the
        // diagnostic carries a real source line. The AST UniformDecl
        // doesn't have its own line/column fields today (uniforms
        // are declared at material scope, which the parser doesn't
        // stamp), so we fall back to whatever location hint the
        // outer driver set via setCurrentLocation().
        error("'" + decl.type +
              "' is not a builtin type (expected: " +
              AYBuiltinTypes::expectedList() + ")",
              _currentLine, _currentColumn, ErrorCode::InvalidOperation);
    }
    // Even on the error path we still register the uniform so later
    // analysis of the body doesn't crash on the dangling identifier.
    // The body might reference the uniform and we don't want to drown
    // the user in "undefined identifier" follow-up errors on top of
    // the type-mismatch error.
    //
    // Prefer a concrete builtin Type (not Dynamic) so material-body
    // `uniform mat4 foo[8]` indexes resolve to mat4 — Dynamic[i]
    // leaves an unresolved TypeVar and BGFX emits typeless `_lvp =`.
    std::shared_ptr<Type> type = BuiltinTypes::Dynamic;
    if (decl.type == "float")      type = BuiltinTypes::Float;
    else if (decl.type == "int")   type = BuiltinTypes::Int;
    else if (decl.type == "uint")  type = BuiltinTypes::Uint;
    else if (decl.type == "bool")  type = BuiltinTypes::Bool;
    else if (decl.type == "vec2")  type = BuiltinTypes::Vec2();
    else if (decl.type == "vec3")  type = BuiltinTypes::Vec3();
    else if (decl.type == "vec4")  type = BuiltinTypes::Vec4();
    else if (decl.type == "mat2")  type = BuiltinTypes::Mat2();
    else if (decl.type == "mat3")  type = BuiltinTypes::Mat3();
    else if (decl.type == "mat4")  type = BuiltinTypes::Mat4();
    if (decl.arrayLength > 0) {
        type = std::make_shared<ArrayType>(
            type, static_cast<size_t>(decl.arrayLength));
    }
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
        // Phase 1 RD-03: skinning vertex attributes are vec4 in Phoskia.
        // Indices bytes (0..255) get re-encoded by the renderer's
        // repack path into bgfx's normalized u8 channel.
        case PhoskiaSemantic::BoneIndices: type = BuiltinTypes::Vec4(); break;
        case PhoskiaSemantic::BoneWeights: type = BuiltinTypes::Vec4(); break;
        case PhoskiaSemantic::Tangent: type = BuiltinTypes::Vec4(); break;
    }
    _env.addVariable(param.name, type);
    _symbols[param.name] = type;
}

void AYSemanticAnalyzer::analyzeVertexFunc(const VertexFunc& func) {
    _env.pushScope();
    _inShaderFunc = true;
    detail::registerFrameBuiltins(_env);
    // IR-H-01 follow-up: anchor the location hint at the `vertex`
    // keyword line so statements inside the block inherit a sensible
    // source position. The hint is restored on exit.
    int savedLine = _currentLine;
    int savedCol = _currentColumn;
    _currentLine = func.line ? func.line : _currentLine;
    _currentColumn = func.column ? func.column : _currentColumn;
    for (const auto& p : func.params) {
        if (auto sp = dynamic_cast<const ShaderParam*>(p.get())) {
            analyzeShaderParam(*sp);
        }
    }
    for (const auto& stmt : func.body) {
        analyze(*stmt);
    }
    _currentLine = savedLine;
    _currentColumn = savedCol;
    _inShaderFunc = false;
    _env.popScope();
}

void AYSemanticAnalyzer::analyzeFragmentFunc(const FragmentFunc& func) {
    _env.pushScope();
    _inShaderFunc = true;
    _fragmentMrtOutputCount = func.outputs.size();
    detail::registerFrameBuiltins(_env);
    // IR-H-01 follow-up: pull the fragment block's source location from
    // the AST node (parser-stamped) so warnings issued for shader-param
    // decls inside it carry a real line number.
    _currentLine = func.line ? func.line : _currentLine;
    _currentColumn = func.column ? func.column : _currentColumn;
    for (const auto& p : func.inputs) {
        if (auto sp = dynamic_cast<const ShaderParam*>(p.get())) {
            analyzeShaderParam(*sp);
        }
    }
    for (const auto& p : func.outputs) {
        if (auto sp = dynamic_cast<const ShaderParam*>(p.get())) {
            // MRT color attachments are vec4 slots; prefer `: color`.
            if (sp->semantic != PhoskiaSemantic::Color) {
                warning("Fragment MRT 'out' should use ': color' (gl_FragData is vec4); "
                        "got semantic for '" + sp->name + "'",
                        _currentLine, _currentColumn, ErrorCode::InvalidOperation);
            }
            analyzeShaderParam(*sp);
            if (sp->defaultValue) {
                analyzeExpr(*sp->defaultValue);
            }
        }
    }
    if (func.outputs.size() > 8) {
        error("Fragment MRT supports at most 8 'out' targets (gl_FragData[0..7])", 0, 0);
    }
    for (const auto& stmt : func.body) {
        analyze(*stmt);
    }
    _fragmentMrtOutputCount = 0;
    _inShaderFunc = false;
    _env.popScope();
}

void AYSemanticAnalyzer::analyzeLetStmt(const LetStmt& stmt) {
    auto type = analyzeExpr(*stmt.initializer);
    _env.addVariable(stmt.name, type);
    _symbols[stmt.name] = type;

    // IR-M-04: record this let-binding so the post-pass walker can flag
    // it as unused if no later expression references `stmt.name`. The
    // location falls back to the current cursor hint when the parser
    // didn't stamp the AST node (IR-H-01: prefer AST-stamped locations).
    DeclRecord rec;
    rec.name = stmt.name;
    rec.line = stmt.line ? stmt.line : _currentLine;
    rec.column = stmt.column ? stmt.column : _currentColumn;
    rec.isLet = true;
    _declaredBindings.push_back(std::move(rec));
}

void AYSemanticAnalyzer::analyzeReturnStmt(const ReturnStmt& stmt) {
    // IR-H-02: prefer the ReturnStmt's own line/column when the parser
    // stamped it; otherwise fall back to the current cursor hint. This
    // is the single biggest improvement from this audit — previous
    // behavior always reported `(0, 0)` even when the AST clearly
    // knew where the bad `return` was.
    int retLine = stmt.line ? stmt.line : _currentLine;
    int retCol = stmt.column ? stmt.column : _currentColumn;

    if (!_inShaderFunc) {
        error("Return statement outside of shader block", retLine, retCol);
        return;
    }
    // Phase 6 #6: MRT fragments write via `out` names → gl_FragData[N].
    // Mixing with return→gl_FragColor is undefined; forbid it.
    if (_fragmentMrtOutputCount > 0) {
        error("Fragment with MRT 'out' targets cannot use 'return'; "
              "assign to the out names instead (maps to gl_FragData[N])",
              retLine, retCol);
        if (stmt.value) {
            (void)analyzeExpr(*stmt.value);
        }
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
            // IR-H-01/IR-H-02: tag with TypeMismatch so diagnostic
            // tooling can group this with other unification errors
            // and report the source line/column of the bad return.
            error("Return type must be vec4 (vertex outputs gl_Position, "
                  "fragment outputs gl_FragColor); got " + concrete->toString(),
                  retLine, retCol, ErrorCode::TypeMismatch);
        }
    }
}

void AYSemanticAnalyzer::analyzeIfStmt(const IfStmt& stmt) {
    // IR-H-04: pull the if-statement's line/column from the AST node
    // (parser-stamped) and fall back to the current cursor hint. The
    // previous code reported `(0, 0)` here unconditionally, which made
    // "I wrote `if (1.0)` somewhere in my 500-line shader" impossible
    // to diagnose. We also tag the error as TypeMismatch for parity
    // with the return-stmt error path (IR-H-01 / IR-H-04).
    int ifLine = stmt.line ? stmt.line : _currentLine;
    int ifCol = stmt.column ? stmt.column : _currentColumn;

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
        error("If condition must be bool; got " + concrete->toString(),
              ifLine, ifCol, ErrorCode::TypeMismatch);
    }

    // IR-M-07: dead-code warning when the condition is a literal `true`
    // or `false`. The unreachable branch (else on `true`, then on `false`)
    // is reported as a warning — runtime semantics still work, but the
    // user almost certainly meant to write a runtime predicate.
    if (auto* lit = dynamic_cast<const LiteralExpr*>(stmt.condition.get())) {
        if (std::holds_alternative<bool>(lit->value)) {
            bool condVal = std::get<bool>(lit->value);
            if (condVal && !stmt.elseBranch.empty()) {
                warning("Unreachable else branch: if-condition is literal `true`",
                        ifLine, ifCol, ErrorCode::InvalidOperation);
            } else if (!condVal && !stmt.thenBranch.empty()) {
                warning("Unreachable then branch: if-condition is literal `false`",
                        ifLine, ifCol, ErrorCode::InvalidOperation);
            }
        }
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
        // IR-M-04: every identifier reference in any later expression
        // counts as a USE of the binding. We increment a usage counter
        // (rather than a set) so the warning walker can distinguish
        // declared-and-read-once (not flagged) from declared-and-zero-read
        // (flagged). The identifier may reference a function name
        // (`normalize`) — we only count identifiers that resolve to
        // variables to keep noise low.
        if (_env.getVariable(id->name)) {
            _usedBindings.insert(id->name);
            _bindingUseCount[id->name] += 1;
        }
    }

    // Delegate to the type-inference engine for the real type. The
    // engine unifies type variables against builtin signatures / env
    // entries and resolves swizzles, array indices, and constructors.
    TypeInference inference(_env);
    auto inferred = inference.infer(expr);

    // IR-H-01 / IR-M-01: cross-check the inferred result against any
    // user-supplied unification hint by walking common binary-op /
    // matrix-multiply patterns the inference engine silently accepts.
    // The inference engine returns a fresh TypeVar on mismatch — we
    // detect that case by checking the *concrete* shapes of matrix /
    // vector operands here, where we have the original AST.
    if (auto bin = dynamic_cast<const BinaryExpr*>(&expr)) {
        if (bin->op.type == TokenType::Star) {
            auto L = inference.infer(*bin->left);
            auto R = inference.infer(*bin->right);
            std::shared_ptr<Type> lConc = resolveTypeVarChain(L);
            std::shared_ptr<Type> rConc = resolveTypeVarChain(R);
            auto lMat = std::dynamic_pointer_cast<MatrixType>(lConc);
            auto rMat = std::dynamic_pointer_cast<MatrixType>(rConc);
            auto lVec = std::dynamic_pointer_cast<VectorType>(lConc);
            auto rVec = std::dynamic_pointer_cast<VectorType>(rConc);
            // mat * vec: requires matrix.cols == vec.dim
            if (lMat && rVec) {
                if (lMat->cols() != rVec->dimension()) {
                    int binLine = bin->line ? bin->line : _currentLine;
                    int binCol = bin->column ? bin->column : _currentColumn;
                    error("Matrix-vector multiply dimension mismatch: "
                          "matrix is " + lConc->toString() + ", vector is " +
                          rConc->toString() + " (expected matrix.cols == vector.dim)",
                          binLine, binCol, ErrorCode::TypeMismatch);
                }
            }
            // vec * mat: requires vec.dim == matrix.rows
            if (lVec && rMat) {
                if (lVec->dimension() != rMat->rows()) {
                    int binLine = bin->line ? bin->line : _currentLine;
                    int binCol = bin->column ? bin->column : _currentColumn;
                    error("Vector-matrix multiply dimension mismatch: "
                          "vector is " + lConc->toString() + ", matrix is " +
                          rConc->toString() + " (expected vector.dim == matrix.rows)",
                          binLine, binCol, ErrorCode::TypeMismatch);
                }
            }
            // mat * mat: requires A.cols == B.rows
            if (lMat && rMat) {
                if (lMat->cols() != rMat->rows()) {
                    int binLine = bin->line ? bin->line : _currentLine;
                    int binCol = bin->column ? bin->column : _currentColumn;
                    error("Matrix-matrix multiply dimension mismatch: "
                          "left is " + lConc->toString() + ", right is " +
                          rConc->toString() + " (expected left.cols == right.rows)",
                          binLine, binCol, ErrorCode::TypeMismatch);
                }
            }
        }
    }

    // IR-M-02: detect out-of-range swizzle axes (e.g. `vec2 v; v.z`).
    // The inference engine returns a fresh TypeVar on the out-of-range
    // path (see inferMemberExpr); we cross-check here against the
    // concrete object type to surface a diagnostic with the
    // MemberExpr's source location.
    if (auto mem = dynamic_cast<const MemberExpr*>(&expr)) {
        auto objT = inference.infer(*mem->object);
        std::shared_ptr<Type> objConc = resolveTypeVarChain(objT);
        if (auto vec = std::dynamic_pointer_cast<VectorType>(objConc)) {
            const std::string& m = mem->member;
            auto axisIndex = [](char c) -> int {
                switch (c) {
                    case 'x': case 'r': return 0;
                    case 'y': case 'g': return 1;
                    case 'z': case 'b': return 2;
                    case 'w': case 'a': return 3;
                    default: return -1;
                }
            };
            for (char c : m) {
                int idx = axisIndex(c);
                if (idx >= static_cast<int>(vec->dimension())) {
                    int mLine = mem->line ? mem->line : _currentLine;
                    int mCol = mem->column ? mem->column : _currentColumn;
                    error("Swizzle axis '" + std::string(1, c) +
                          "' is out of range for " + vec->toString() +
                          " (dimension " + std::to_string(vec->dimension()) + ")",
                          mLine, mCol, ErrorCode::TypeMismatch);
                    break;  // report first error only — avoid noise
                }
            }
        }
    }

    // IR-M-03: literal integer divide-by-zero detection. We only flag
    // the literal case (two IntLiteral operands on a `/`) — runtime
    // division by zero is the GPU's responsibility (GLSL spec: undefined).
    // Real users write `1/0` only by accident (e.g. from copy-pasted
    // code), and the surface diagnostic helps locate the bug fast.
    if (auto bin = dynamic_cast<const BinaryExpr*>(&expr)) {
        if (bin->op.type == TokenType::Slash) {
            auto* rLit = dynamic_cast<const LiteralExpr*>(bin->right.get());
            if (rLit && std::holds_alternative<int>(rLit->value) &&
                std::get<int>(rLit->value) == 0) {
                int binLine = bin->line ? bin->line : _currentLine;
                int binCol = bin->column ? bin->column : _currentColumn;
                warning("Division by zero (integer literal)",
                        binLine, binCol, ErrorCode::InvalidOperation);
            }
        }
    }

    // IR-H-03: builtin-function arity check. The inference engine
    // returns a fresh TypeVar when no overload matches by arity OR
    // signature — silently. Detect that case for CallExpr nodes whose
    // callee is a known builtin name. We don't try to match the full
    // overload signature (that's the inference engine's job and it
    // already does best-effort scoring) — we just check arity, which
    // is the user-visible mistake ("normalize(v, v) — wait, that takes
    // one arg").
    if (auto call = dynamic_cast<const CallExpr*>(&expr)) {
        if (auto id = dynamic_cast<const IdentifierExpr*>(call->callee.get())) {
            const std::string& name = id->name;
            auto& reg = BuiltinFunctionRegistry::instance();
            // Vector and matrix constructors are component-based rather than
            // fixed-arity functions. For example, vec4 accepts (vec3, float),
            // (vec2, float, float), or four scalars. TypeInference validates
            // those through inferConstructor(), so applying the registry's
            // canonical scalar signature here produces a false arity error.
            if (reg.hasFunction(name) && !isCompositeTypeConstructor(name)) {
                auto* ovls = reg.getOverloads(name);
                bool anyArityMatches = false;
                if (ovls) {
                    for (const auto& ov : *ovls) {
                        if (ov.paramTypes.size() == call->args.size()) {
                            anyArityMatches = true;
                            break;
                        }
                    }
                }
                if (!anyArityMatches && !call->args.empty()) {
                    int callLine = call->line ? call->line : _currentLine;
                    int callCol = call->column ? call->column : _currentColumn;
                    std::string msg = "Builtin function '" + name +
                        "' has no overload taking " +
                        std::to_string(call->args.size()) + " argument(s)";
                    if (ovls && !ovls->empty()) {
                        msg += " (registered overloads take ";
                        for (size_t i = 0; i < ovls->size(); ++i) {
                            if (i > 0) msg += ", ";
                            msg += std::to_string((*ovls)[i].paramTypes.size());
                        }
                        msg += ")";
                    }
                    error(msg, callLine, callCol, ErrorCode::TypeMismatch);
                }
            }
        }
    }

    // IR-M-05: detect int→float implicit conversion in type
    // constructors like `vec3(int_val)` where the argument is a known
    // int literal and the constructor expects float. The inference
    // engine unifies them silently; the GLSL backend emits the call
    // and shaderc errors with a confusing message. We catch it here
    // at the analyzer boundary where we still have the source
    // location for the call site.
    if (auto call = dynamic_cast<const CallExpr*>(&expr)) {
        if (auto id = dynamic_cast<const IdentifierExpr*>(call->callee.get())) {
            const std::string& name = id->name;
            bool isFloatCtor = (name == "vec2" || name == "vec3" || name == "vec4" ||
                                name == "mat2" || name == "mat3" || name == "mat4");
            bool isIntCtor   = (name == "ivec2" || name == "ivec3" || name == "ivec4");
            if (isFloatCtor && call->args.size() == 1) {
                auto* argLit = dynamic_cast<const LiteralExpr*>(call->args[0].get());
                if (argLit && std::holds_alternative<int>(argLit->value)) {
                    int cLine = call->line ? call->line : _currentLine;
                    int cCol = call->column ? call->column : _currentColumn;
                    warning("Implicit int→float conversion in '" + name +
                            "' constructor; consider using 'float(...)' or 'ivec' form",
                            cLine, cCol, ErrorCode::TypeMismatch);
                }
            }
            // Cross: float arg into ivec constructor is the inverse
            // error, also flag it.
            if (isIntCtor && call->args.size() == 1) {
                auto* argLit = dynamic_cast<const LiteralExpr*>(call->args[0].get());
                if (argLit && std::holds_alternative<float>(argLit->value)) {
                    int cLine = call->line ? call->line : _currentLine;
                    int cCol = call->column ? call->column : _currentColumn;
                    warning("Implicit float→int conversion in '" + name +
                            "' constructor; consider using 'int(...)' or 'vec' form",
                            cLine, cCol, ErrorCode::TypeMismatch);
                }
            }
        }
    }

    return inferred;
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

void AYSemanticAnalyzer::error(const std::string& message, int line, int column,
                                  ErrorCode code) {
    // IR-H-01..04: tag the error with the correct ErrorCode (default
    // UnknownIdentifier for backward compatibility — but every existing
    // call site has been audited and now passes the appropriate code).
    std::cerr << "[SemanticAnalyzer] error at line=" << line
              << " col=" << column << ": " << message << "\n";
    _reporter.error(code, message, line, column);
}

void AYSemanticAnalyzer::warning(const std::string& message, int line, int column,
                                  ErrorCode code) {
    _warnings.emplace_back(code, message, line, column);
    std::cerr << "[SemanticAnalyzer] warning at line=" << line
              << " col=" << column << ": " << message << "\n";
}

void AYSemanticAnalyzer::emitUnusedBindingWarnings() {
    // IR-M-04: walk _declaredBindings and flag every let-stmt that
    // was never read by a later expression. Material properties and
    // uniforms are intentionally NOT flagged — they're public API
    // surface that may be referenced by other shaders / driver code.
    // The check uses _usedBindings populated by analyzeExpr.
    for (const auto& rec : _declaredBindings) {
        if (!rec.isLet) continue;
        if (_usedBindings.count(rec.name) != 0) continue;
        // Don't flag the trivial case where the binding is the very
        // last stmt in a body — those are typically the value the
        // caller wanted but forgot to read (we still warn to nudge).
        warning("Unused binding '" + rec.name + "' (declared but never read)",
                rec.line, rec.column, ErrorCode::InvalidOperation);
    }
}

bool AYSemanticAnalyzer::isDefined(const std::string& name) const {
    return _symbols.find(name) != _symbols.end();
}

std::shared_ptr<Type> AYSemanticAnalyzer::getType(const std::string& name) const {
    auto it = _symbols.find(name);
    return (it != _symbols.end()) ? it->second : nullptr;
}

} // namespace ayt::shader::phoskia
