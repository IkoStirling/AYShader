// AYIr.cpp - Phoskia IR Generator implementation
//
// Phase 3.1: lowers a parsed Phoskia Program (AST) into an IRProgram.
// The IR is a 1:1 AST mirror with `resolvedType` pre-attached to every
// expression. Backends consume the IR instead of the AST.
//
// Type resolution strategy:
//   - If a TypeEnvironment is supplied (from SemanticAnalyzer / pipeline),
//     we look up IdentifierExpr names in the env directly.
//   - Otherwise we run a fresh TypeInference per expression. This is the
//     graceful-degradation path: the IRGenerator always produces types,
//     even when the user did not enable Phase 2's analyzeSemantics step.

#include "AYIr.h"
#include "AYTypeInference.h"
#include "AYBuiltinFunctions.h"
#include "detail/AYPhoskiaFrameBuiltins.h"
#include <unordered_map>

namespace ayt::shader::phoskia::ir
{

namespace {

// Map a GLSL / Phoskia type lexeme to the corresponding Type shared_ptr.
// Mirrors the table at src/AYBGFXConverter.cpp:89-104.
std::shared_ptr<Type> lexemeToType(const std::string& lex) {
    using namespace phoskia;
    if (lex == "float")  return BuiltinTypes::Float;
    if (lex == "int")    return BuiltinTypes::Int;
    if (lex == "uint")   return BuiltinTypes::Uint;  // Phase 3.3 Block 1
    if (lex == "bool")   return BuiltinTypes::Bool;
    if (lex == "vec2")   return BuiltinTypes::Vec2();
    if (lex == "vec3")   return BuiltinTypes::Vec3();
    if (lex == "vec4")   return BuiltinTypes::Vec4();
    if (lex == "ivec2")  return std::make_shared<VectorType>(PrimitiveType::Int, 2);
    if (lex == "ivec3")  return std::make_shared<VectorType>(PrimitiveType::Int, 3);
    if (lex == "ivec4")  return std::make_shared<VectorType>(PrimitiveType::Int, 4);
    if (lex == "mat2")   return BuiltinTypes::Mat2();
    if (lex == "mat3")   return BuiltinTypes::Mat3();
    if (lex == "mat4")   return BuiltinTypes::Mat4();
    return nullptr;
}

// Walk a TypeVar solution chain to its concrete root. Mirrors the cycle-
// detection loop at src/AYTypeInference.cpp:22-35 (TypeInference already
// has one internally, but the IRGenerator runs on the result so we need
// to follow solutions again here).
std::shared_ptr<Type> resolveChain(std::shared_ptr<Type> t) {
    auto last = t;
    int hops = 0;
    while (auto tv = std::dynamic_pointer_cast<TypeVar>(last)) {
        if (!tv->hasSolution()) break;
        auto next = tv->getSolution();
        if (next == last || next == t) return last;
        if (++hops > 64) return last;
        last = next;
    }
    return last;
}

} // namespace

// --------------------------------------------------------------------------
// TypeEnvironment helpers (lazily populated)
// --------------------------------------------------------------------------

// Populate a TypeEnvironment with the builtin function registry so that
// TypeInference::infer can resolve calls to vec3(...), normalize(...),
// fresnelSchlick(...), etc. without the caller having run SemanticAnalyzer.
// Used when the IRGenerator is invoked without a pre-built env.
static void populateBuiltinEnv(phoskia::TypeEnvironment& env) {
    using namespace phoskia;
    auto& reg = BuiltinFunctionRegistry::instance();
    for (const auto& name : reg.getAllFunctionNames()) {
        auto func = reg.getFunction(name);
        if (func) {
            // Phase 3.3 Block 3: zero-arg builtins (thread_id /
            // group_id / dispatch_id / ...) are exposed in Phoskia as
            // bare identifiers (`thread_id.x` rather than `thread_id().x`).
            // When their FunctionType is added to the env,
            // `inferIdentifierExpr` resolves them to FunctionType — which
            // then breaks `inferMemberExpr` (it expects a VectorType, not
            // a FunctionType, for swizzle inference).
            //
            // The fix is to skip 0-arg builtins here so
            // `inferIdentifierExpr` falls through to its Phase 3.2
            // builtin-registry fallback (which returns the return
            // type directly — uvec3 today). With this skip, the
            // strict-typed uvec3 / uint chain lights up for
            // `thread_id.x` swizzles end-to-end.
            //
            // Multi-arg builtins (vec3, normalize, dot, ...) still need
            // to be in the env so `inferCallExpr`'s FunctionType path
            // can resolve them when the identifier is followed by `(`.
            if (func->paramTypes.empty()) continue;
            env.addFunction(name,
                std::make_shared<FunctionType>(func->paramTypes, func->returnType));
        }
    }
}

// --------------------------------------------------------------------------
// IRGenerator
// --------------------------------------------------------------------------

std::shared_ptr<Type> IRGenerator::resolveTypeVar(std::shared_ptr<Type> t) const {
    return resolveChain(t);
}

std::shared_ptr<Type> IRGenerator::typeFromGLSLLexeme(const std::string& lex) const {
    return lexemeToType(lex);
}

std::shared_ptr<Type> IRGenerator::resolveType(const phoskia::Expr& e,
                                                phoskia::TypeEnvironment* scopeEnv) {
    // The scope env (when provided) is already populated with builtin
    // functions AND per-block let-bindings by the caller. Pass it
    // directly to TypeInference so identifier lookups for D/G/F/etc.
    // resolve to their concrete types instead of fresh TypeVars.
    if (scopeEnv) {
        phoskia::TypeInference inference(*scopeEnv);
        auto inferred = inference.infer(e);
        return resolveTypeVar(inferred);
    }
    // No per-block scope — fall back to a fresh builtin-only env.
    phoskia::TypeEnvironment local;
    populateBuiltinEnv(local);
    if (_env && _env->hasVariable(std::string{})) {
        // (kept for clarity; not used in practice)
    }
    // Identifier fast-path: if the caller supplied a SemanticAnalyzer
    // env and the expression is a plain identifier, look it up
    // directly without running TypeInference (saves the inference
    // pass and avoids losing builtin info).
    if (auto id = dynamic_cast<const phoskia::IdentifierExpr*>(&e)) {
        if (_env && _env->hasVariable(id->name)) {
            return resolveTypeVar(_env->getVariable(id->name));
        }
    }
    phoskia::TypeInference inference(local);
    auto inferred = inference.infer(e);
    return resolveTypeVar(inferred);
}

IRProgram IRGenerator::generate(const phoskia::Program& ast,
                                std::shared_ptr<phoskia::TypeEnvironment> typeEnv) {
    IRProgram out;
    _warnings.clear();
    _env = typeEnv;
    // (Phase 3.4 had a `nextBinding_ = 0;` reset here for the
    // per-program UBO slot counter. Phase 3.5-B removed the counter;
    // binding slots are now resolved at BGFX emit time. The IR
    // carries the user's `binding` literal (or -1 for "auto") in
    // each IRDeclaration's uboBinding field.)

    for (const auto& decl : ast.declarations) {
        if (auto mat = dynamic_cast<const phoskia::MaterialDecl*>(decl.get())) {
            auto ir = lowerMaterialDecl(*mat);
            if (ir) out.materials.push_back(std::move(ir));
        } else if (auto cmp = dynamic_cast<const phoskia::ComputeDecl*>(decl.get())) {
            auto ir = lowerComputeDecl(*cmp);
            if (ir) out.computes.push_back(std::move(ir));
        } else if (auto ub = dynamic_cast<const phoskia::UniformBlockDecl*>(decl.get())) {
            // Phase 3.4: top-level uniform buffer object. The block
            // lives in IRProgram::uniformBlocks (shared across
            // materials / computes in the same source file) rather
            // than in any one material's declarations vector.
            auto ir = lowerDecl(*ub);
            if (ir) out.uniformBlocks.push_back(std::move(ir));
        }
        // Program-level declarations other than Material/Compute/UniformBlock
        // are unknown — log a warning and skip.
    }

    out.warnings = std::move(_warnings);
    _env.reset();
    return out;
}

// --------------------------------------------------------------------------
// Lowering: declarations
// --------------------------------------------------------------------------

std::unique_ptr<IRDeclaration> IRGenerator::lowerDecl(const phoskia::Stmt& s) {
    auto out = std::make_unique<IRDeclaration>();
    if (auto u = dynamic_cast<const phoskia::UniformDecl*>(&s)) {
        out->kind = IRDeclaration::Kind::Uniform;
        out->name = u->name;
        out->uniformType = lexemeToType(u->type);
        if (!out->uniformType) {
            _warnings.push_back("Uniform '" + u->name +
                "' has unrecognized GLSL type lexeme '" + u->type +
                "'; IR will not carry a Type pointer for it");
        }
    } else if (auto p = dynamic_cast<const phoskia::PropertyDecl*>(&s)) {
        out->kind = IRDeclaration::Kind::Property;
        out->name = p->name;
        if (p->initializer) {
            out->propertyInit = lowerExpr(*p->initializer);
        }
    } else if (auto t = dynamic_cast<const phoskia::TextureDecl*>(&s)) {
        out->kind = IRDeclaration::Kind::Texture;
        out->name = t->name;
        switch (t->samplerKind) {
        case phoskia::TextureSamplerKind::SamplerCube:
            out->samplerKind = SamplerKind::SamplerCube;
            break;
        case phoskia::TextureSamplerKind::Sampler2D:
        default:
            out->samplerKind = SamplerKind::Sampler2D;
            break;
        }
    } else if (auto st = dynamic_cast<const phoskia::StorageDecl*>(&s)) {
        // Phase 3.2 Block 3: compute storage buffer.
        // The element type is carried as a Type pointer (target-neutral)
        // so backends can emit GLSL / HLSL / WGSL with the right GLSL
        // lexeme. Custom struct element types are a Phase 3.3 extension;
        // for now lexemeToType covers builtin scalar / vector types and
        // falls through with a warning for anything else.
        out->kind = IRDeclaration::Kind::Storage;
        out->name = st->name;
        out->storageElementType = lexemeToType(st->elementType);
        out->storageAccess = (st->access == phoskia::StorageDecl::Access::Read)
                                 ? IRDeclaration::StorageAccess::Read
                                 : IRDeclaration::StorageAccess::ReadWrite;
        // Phase 3.5-A: propagate explicit binding slot if user wrote
        // `storage X : rwstructuredbuffer<T> binding N;`. The default
        // -1 (no binding) keeps the historical auto-assign path live.
        // The BGFX backend reads this field to emit
        // `layout(std430, binding = N)` and to detect duplicate
        // bindings across the same compute's decls.
        out->storageBinding = st->binding;
        if (!out->storageElementType) {
            _warnings.push_back("Storage '" + st->name +
                "' has unrecognized element type lexeme '" + st->elementType +
                "'; BGFX emission will fall back to vec4");
        }
    } else if (auto sh = dynamic_cast<const phoskia::SharedDecl*>(&s)) {
        // Phase 3.3 Block 4: workgroup-shared local memory.
        // Mirrors StorageDecl's element-type plumbing. The size is
        // already a literal int on the AST node (parser enforces),
        // so we just copy it forward — no expression lowering needed.
        out->kind = IRDeclaration::Kind::Shared;
        out->name = sh->name;
        out->sharedElementType = lexemeToType(sh->elementType);
        out->sharedSize = sh->size;
        if (!out->sharedElementType) {
            _warnings.push_back("Shared '" + sh->name +
                "' has unrecognized element type lexeme '" + sh->elementType +
                "'; BGFX emission will fall back to vec4");
        }
    } else if (auto ub = dynamic_cast<const phoskia::UniformBlockDecl*>(&s)) {
        // Phase 3.4: top-level uniform buffer object.
        // Each field's type lexeme resolves through the same
        // lexemeToType table as StorageDecl / SharedDecl (builtin
        // scalar / vector / matrix forms). Unknown lexemes warn
        // and fall back to vec4 — same fallback policy as the other
        // "emits a GLSL type lexeme" decls.
        //
        // Binding slot (Phase 3.5-B): propagated verbatim from the
        // AST's `ub->binding` field. -1 = "no explicit binding;
        // backend auto-assigns at emit time" (Phase 3.4 historical
        // behavior). >= 0 = literal user-written binding.
        //
        // Note: known limitation — we don't register the UBO block
        // name (e.g. `Camera`) or its field types in the body's
        // TypeEnvironment, so `let p = Camera.position` infers `p`
        // as a fresh TypeVar rather than vec3. The emit path is
        // unaffected (emitExpr(MemberExpr, ...) writes
        // `Camera.position` literally); GLSL-side type errors are
        // reported by shaderc. Type-checking the body against UBO
        // fields is a Phase 4+ task tied to struct type inference.
        out->kind = IRDeclaration::Kind::UniformBlock;
        out->name = ub->name;
        out->uboFieldNames.reserve(ub->fields.size());
        out->uboFields.reserve(ub->fields.size());
        for (const auto& f : ub->fields) {
            out->uboFieldNames.push_back(f.name);
            auto t = lexemeToType(f.type);
            if (!t) {
                _warnings.push_back("UniformBlock '" + ub->name + "' field '" + f.name +
                    "' has unrecognized type lexeme '" + f.type +
                    "'; BGFX emission will fall back to vec4");
                t = phoskia::BuiltinTypes::Vec4();
            }
            out->uboFields.push_back(t);
        }
        // Phase 3.5-B: pass through AST binding (no IR-side counter).
        out->uboBinding = ub->binding;
    } else {
        return nullptr;
    }
    return out;
}

// --------------------------------------------------------------------------
// Lowering: shader params
// --------------------------------------------------------------------------

std::unique_ptr<IRShaderParam> IRGenerator::lowerShaderParam(const phoskia::ShaderParam& p,
                                                             phoskia::TypeEnvironment* scopeEnv) {
    auto out = std::make_unique<IRShaderParam>();
    out->dir = (p.dir == phoskia::ShaderParam::Direction::In)
                   ? IRShaderParam::Direction::In
                   : IRShaderParam::Direction::Out;
    out->name = p.name;
    out->semantic = p.semantic;
    if (p.defaultValue) {
        out->defaultValue = lowerExpr(*p.defaultValue, scopeEnv);
    }
    return out;
}

// --------------------------------------------------------------------------
// Lowering: vertex/fragment/material/compute
// --------------------------------------------------------------------------

std::unique_ptr<IRVertexFunc> IRGenerator::lowerVertexFuncWithEnv(const phoskia::VertexFunc& vf,
                                                                  phoskia::TypeEnvironment& env) {
    auto out = std::make_unique<IRVertexFunc>();
    detail::registerFrameBuiltins(env);
    for (const auto& s : vf.params) {
        if (auto p = dynamic_cast<const phoskia::ShaderParam*>(s.get())) {
            auto sp = lowerShaderParam(*p, &env);
            if (sp) {
                // Register in/out param names so subsequent body lets
                // can resolve `let nrm = normalize(nrm)` correctly.
                // Semantic → GLSL type mapping mirrors the BGFX
                // converter's semanticTable() at src/AYBGFXConverter.cpp:55.
                static const std::unordered_map<phoskia::PhoskiaSemantic, std::shared_ptr<Type>> semType = {
                    {phoskia::PhoskiaSemantic::Position,  BuiltinTypes::Vec3()},
                    {phoskia::PhoskiaSemantic::Normal,    BuiltinTypes::Vec3()},
                    {phoskia::PhoskiaSemantic::Color,     BuiltinTypes::Vec4()},
                    {phoskia::PhoskiaSemantic::Texcoord,  BuiltinTypes::Vec2()},
                };
                auto it = semType.find(p->semantic);
                if (it != semType.end()) env.addVariable(p->name, it->second);
                out->params.push_back(std::move(sp));
            }
        }
        // non-ShaderParam entries in params should not appear (parser
        // enforces this); if they do, skip silently.
    }
    for (const auto& s : vf.body) {
        auto ir = lowerStmt(*s, &env);
        if (ir) out->body.push_back(std::move(ir));
    }
    return out;
}

std::unique_ptr<IRFragmentFunc> IRGenerator::lowerFragmentFuncWithEnv(const phoskia::FragmentFunc& ff,
                                                                      phoskia::TypeEnvironment& env) {
    auto out = std::make_unique<IRFragmentFunc>();
    detail::registerFrameBuiltins(env);
    for (const auto& s : ff.inputs) {
        if (auto p = dynamic_cast<const phoskia::ShaderParam*>(s.get())) {
            auto sp = lowerShaderParam(*p, &env);
            if (sp) {
                static const std::unordered_map<phoskia::PhoskiaSemantic, std::shared_ptr<Type>> semType = {
                    {phoskia::PhoskiaSemantic::Position,  BuiltinTypes::Vec3()},
                    {phoskia::PhoskiaSemantic::Normal,    BuiltinTypes::Vec3()},
                    {phoskia::PhoskiaSemantic::Color,     BuiltinTypes::Vec4()},
                    {phoskia::PhoskiaSemantic::Texcoord,  BuiltinTypes::Vec2()},
                };
                auto it = semType.find(p->semantic);
                if (it != semType.end()) env.addVariable(p->name, it->second);
                out->inputs.push_back(std::move(sp));
            }
        }
    }
    for (const auto& s : ff.body) {
        auto ir = lowerStmt(*s, &env);
        if (ir) out->body.push_back(std::move(ir));
    }
    return out;
}

std::unique_ptr<IRMaterialDecl> IRGenerator::lowerMaterialDecl(const phoskia::MaterialDecl& m) {
    auto out = std::make_unique<IRMaterialDecl>();
    out->name = m.name;
    // Pre-build per-block envs that mirror the BGFX converter's previous
    // vsEnv / fsEnv accumulation. Each env starts with the builtin
    // function registry + material-level uniform/property/texture
    // bindings, then lowerVertexFunc / lowerFragmentFunc add their
    // in/out ShaderParam bindings and let-stmt bindings.
    auto seedEnv = [&]() {
        phoskia::TypeEnvironment env;
        populateBuiltinEnv(env);
        return env;
    };
    phoskia::TypeEnvironment vsSeed = seedEnv();
    phoskia::TypeEnvironment fsSeed = seedEnv();
    for (const auto& d : m.declarations) {
        if (auto u = dynamic_cast<const phoskia::UniformDecl*>(d.get())) {
            auto t = lexemeToType(u->type);
            if (t) {
                vsSeed.addVariable(u->name, t);
                fsSeed.addVariable(u->name, t);
            }
        } else if (auto p = dynamic_cast<const phoskia::PropertyDecl*>(d.get())) {
            // Property types come from the initializer — resolve it
            // before adding to the seed envs. The expression is a
            // simple literal at this point (vector / float / etc.); no
            // body lets are in scope yet.
            if (p->initializer) {
                auto propType = resolveType(*p->initializer, &vsSeed);
                if (propType) {
                    vsSeed.addVariable(p->name, propType);
                    fsSeed.addVariable(p->name, propType);
                }
            }
        } else if (auto t = dynamic_cast<const phoskia::TextureDecl*>(d.get())) {
            vsSeed.addVariable(t->name, phoskia::BuiltinTypes::Dynamic);
            fsSeed.addVariable(t->name, phoskia::BuiltinTypes::Dynamic);
        }
    }
    for (const auto& d : m.declarations) {
        if (auto ir = lowerDecl(*d)) {
            out->declarations.push_back(std::move(ir));
        } else if (auto vf = dynamic_cast<const phoskia::VertexFunc*>(d.get())) {
            // Clone the seed env for this vertex block so let-bindings
            // don't leak into fragment (or vice versa).
            phoskia::TypeEnvironment blockEnv = vsSeed;
            out->vertex = lowerVertexFuncWithEnv(*vf, blockEnv);
        } else if (auto ff = dynamic_cast<const phoskia::FragmentFunc*>(d.get())) {
            phoskia::TypeEnvironment blockEnv = fsSeed;
            out->fragment = lowerFragmentFuncWithEnv(*ff, blockEnv);
        }
        // VariantAttribute at material level is ignored (it lives in
        // body, not the top-level decl list). Same as BGFX converter.
    }
    return out;
}

std::unique_ptr<IRComputeDecl> IRGenerator::lowerComputeDecl(const phoskia::ComputeDecl& c) {
    auto out = std::make_unique<IRComputeDecl>();
    out->name = c.name;
    // Phase 3.3 Block 2: forward the optional [numthreads(X, Y, Z)]
    // attribute from the AST node to the IR node. The BGFX backend
    // reads out->hasNumThreads and out->numThreads to drive the
    // `layout(local_size_x = N, ...)` directive emission.
    out->hasNumThreads = c.hasNumThreads;
    out->numThreads = c.numThreads;
    phoskia::TypeEnvironment env;
    populateBuiltinEnv(env);

    // Phase 3.2 Block 3: walk the body twice — once for declarations
    // (storage buffers today; uniform/property in the future), once
    // for statements. The first pass populates `declarations` so the
    // BGFX converter can emit them as `buffer Name { T data[]; } Name;`
    // blocks before the `void main()` body. Mirrors the
    // lowerMaterialDecl pattern (Phase 3.1).
    for (const auto& s : c.body) {
        if (auto decl = lowerDecl(*s)) {
            out->declarations.push_back(std::move(decl));
        }
    }
    for (const auto& s : c.body) {
        // Skip decl-typed entries (already handled above) — lowerDecl
        // returns non-null for Uniform/Property/Texture/Storage/Shared.
        if (dynamic_cast<const phoskia::UniformDecl*>(s.get()) ||
            dynamic_cast<const phoskia::PropertyDecl*>(s.get()) ||
            dynamic_cast<const phoskia::TextureDecl*>(s.get()) ||
            dynamic_cast<const phoskia::StorageDecl*>(s.get()) ||
            dynamic_cast<const phoskia::SharedDecl*>(s.get())) {
            continue;
        }
        auto ir = lowerStmt(*s, &env);
        if (ir) out->body.push_back(std::move(ir));
    }
    return out;
}

// --------------------------------------------------------------------------
// Lowering: statements
// --------------------------------------------------------------------------

std::unique_ptr<IRStmt> IRGenerator::lowerStmt(const phoskia::Stmt& s,
                                              phoskia::TypeEnvironment* scopeEnv) {
    if (auto let = dynamic_cast<const phoskia::LetStmt*>(&s)) {
        auto out = std::make_unique<IRLetStmt>(let->name, nullptr);
        if (let->initializer) {
            out->initializer = lowerExpr(*let->initializer, scopeEnv);
        }
        // Register the let-stmt's resolved type in the scope env so
        // subsequent `let y = let_x * 2.0` resolves `let_x` to its
        // concrete type (vector / matrix / primitive) rather than a
        // fresh TypeVar. Mirrors the BGFX converter's previous per-block
        // vsEnv/fsEnv accumulation pattern.
        if (scopeEnv && out->initializer && out->initializer->resolvedType) {
            scopeEnv->addVariable(let->name, out->initializer->resolvedType);
        }
        return out;
    }
    if (auto ret = dynamic_cast<const phoskia::ReturnStmt*>(&s)) {
        auto out = std::make_unique<IRReturnStmt>(nullptr);
        if (ret->value) {
            out->value = lowerExpr(*ret->value, scopeEnv);
        }
        return out;
    }
    if (auto es = dynamic_cast<const phoskia::ExprStmt*>(&s)) {
        auto out = std::make_unique<IRExprStmt>(nullptr);
        if (es->expr) {
            out->expr = lowerExpr(*es->expr, scopeEnv);
        }
        return out;
    }
    if (auto va = dynamic_cast<const phoskia::VariantAttribute*>(&s)) {
        return std::make_unique<IRVariantAttribute>(va->name);
    }
    if (auto iff = dynamic_cast<const phoskia::IfStmt*>(&s)) {
        auto out = std::make_unique<IRIfStmt>();
        if (iff->condition) out->condition = lowerExpr(*iff->condition, scopeEnv);
        for (const auto& t : iff->thenBranch) {
            auto ir = lowerStmt(*t, scopeEnv);
            if (ir) out->thenBranch.push_back(std::move(ir));
        }
        for (const auto& e : iff->elseBranch) {
            auto ir = lowerStmt(*e, scopeEnv);
            if (ir) out->elseBranch.push_back(std::move(ir));
        }
        return out;
    }
    if (auto fo = dynamic_cast<const phoskia::ForStmt*>(&s)) {
        auto out = std::make_unique<IRForStmt>();
        out->variable = fo->variable;
        if (fo->iterable) out->iterable = lowerExpr(*fo->iterable, scopeEnv);
        for (const auto& b : fo->body) {
            auto ir = lowerStmt(*b, scopeEnv);
            if (ir) out->body.push_back(std::move(ir));
        }
        return out;
    }
    // ShaderParam at body level: should not happen (parser puts them in
    // params/inputs). Skip if seen.
    return nullptr;
}

// --------------------------------------------------------------------------
// Lowering: expressions
// --------------------------------------------------------------------------

std::unique_ptr<IRExpr> IRGenerator::lowerExpr(const phoskia::Expr& e,
                                                phoskia::TypeEnvironment* scopeEnv) {
    std::unique_ptr<IRExpr> out;
    if (auto bin = dynamic_cast<const phoskia::BinaryExpr*>(&e)) {
        auto left = bin->left ? lowerExpr(*bin->left, scopeEnv) : nullptr;
        auto right = bin->right ? lowerExpr(*bin->right, scopeEnv) : nullptr;
        auto ir = std::make_unique<IRBinaryExpr>(std::move(left), bin->op, std::move(right));
        ir->resolvedType = resolveType(e, scopeEnv);
        return ir;
    }
    if (auto un = dynamic_cast<const phoskia::UnaryExpr*>(&e)) {
        auto operand = un->operand ? lowerExpr(*un->operand, scopeEnv) : nullptr;
        auto ir = std::make_unique<IRUnaryExpr>(un->op, std::move(operand));
        ir->resolvedType = resolveType(e, scopeEnv);
        return ir;
    }
    if (auto call = dynamic_cast<const phoskia::CallExpr*>(&e)) {
        auto callee = call->callee ? lowerExpr(*call->callee, scopeEnv) : nullptr;
        std::vector<IRExprPtr> args;
        args.reserve(call->args.size());
        for (const auto& a : call->args) {
            if (a) args.push_back(lowerExpr(*a, scopeEnv));
            else   args.push_back(nullptr);
        }
        auto ir = std::make_unique<IRCallExpr>(std::move(callee), std::move(args));
        ir->resolvedType = resolveType(e, scopeEnv);
        return ir;
    }
    if (auto id = dynamic_cast<const phoskia::IdentifierExpr*>(&e)) {
        auto ir = std::make_unique<IRIdentifierExpr>(id->name);
        ir->resolvedType = resolveType(e, scopeEnv);
        return ir;
    }
    if (auto lit = dynamic_cast<const phoskia::LiteralExpr*>(&e)) {
        auto ir = std::make_unique<IRLiteralExpr>(lit->value);
        ir->resolvedType = resolveType(e, scopeEnv);
        return ir;
    }
    if (auto mem = dynamic_cast<const phoskia::MemberExpr*>(&e)) {
        auto object = mem->object ? lowerExpr(*mem->object, scopeEnv) : nullptr;
        auto ir = std::make_unique<IRMemberExpr>(std::move(object), mem->member);
        ir->resolvedType = resolveType(e, scopeEnv);
        return ir;
    }
    if (auto idx = dynamic_cast<const phoskia::IndexExpr*>(&e)) {
        auto object = idx->object ? lowerExpr(*idx->object, scopeEnv) : nullptr;
        auto index  = idx->index  ? lowerExpr(*idx->index,  scopeEnv) : nullptr;
        auto ir = std::make_unique<IRIndexExpr>(std::move(object), std::move(index));
        ir->resolvedType = resolveType(e, scopeEnv);
        return ir;
    }
    return nullptr;
}

} // namespace ayt::shader::phoskia::ir