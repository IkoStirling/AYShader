#pragma once
// AYIr.h - Phoskia Intermediate Representation (IR)
//
// Phase 3.1 introduces a target-neutral IR layer between the Phoskia AST
// and backend converters. The IR is a 1:1 mirror of the AST nodes — every
// AST node has exactly one IR node counterpart — with one key addition:
// every IR expression carries a `resolvedType` populated at IR-generation
// time. Backends consume the IR instead of the AST; they read types from
// the pre-resolved fields rather than re-running type inference.
//
// This is deliberately NOT SSA-style. SSA (Load/Store/Add/Mul/Phi/BasicBlock)
// requires deciding memory model, dominance frontiers, and phi placement,
// which is real design work for an optimization pass. The AST-mirror IR is
// sufficient for backend emission today; if cross-backend optimization
// (constant folding, dead-code elimination) becomes a goal, an SSA layer
// can be added ON TOP of this IR.
//
// Class layout follows AYAst.h: forward declarations first, then base
// classes, then concrete node classes, then accept() out-of-line.

#include "AYAst.h"   // PhoskiaSemantic, Token reused for IRBinaryExpr.op etc.
#include "AYType.h"
#include <array>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

namespace ayt::shader::phoskia::ir
{

// (1) Forward declarations
class IRNode;
class IRExpr;
class IRStmt;
class IRDeclaration;

class IRBinaryExpr;
class IRUnaryExpr;
class IRCallExpr;
class IRIdentifierExpr;
class IRLiteralExpr;
class IRMemberExpr;
class IRIndexExpr;

class IRLetStmt;
class IRReturnStmt;
class IRIfStmt;
class IRForStmt;
class IRExprStmt;
class IRVariantAttribute;
class IRShaderParam;
class IRVertexFunc;
class IRFragmentFunc;
class IRMaterialDecl;
class IRComputeDecl;

// Sampler kind for texture declarations. The AST's TextureDecl has no
// sampler-kind field (BGFX hardcodes sampler2D), but the IR is the right
// place to carry it so HLSL (Texture2D + SamplerState) and WGSL
// (texture_2d<f32>) backends can map correctly. The IRGenerator infers
// Sampler2D from the existing `texture2d` keyword today; future texture
// types add new lexer keywords without IR changes.
enum class SamplerKind : uint8_t {
    Sampler2D,
    Sampler3D,
    SamplerCube,
};

// (2) Base class
class IRNode {
public:
    virtual ~IRNode() = default;
};

// (3) Concrete Expr nodes — all carry resolvedType.
class IRExpr : public IRNode {
public:
    virtual ~IRExpr() = default;
    // Resolved at IR-generation time. nullptr means the IRGenerator could
    // not infer a concrete type (e.g. unresolved identifier). Backends
    // must treat nullptr as "unknown" and fall back to defaults.
    std::shared_ptr<Type> resolvedType;
};
using IRExprPtr = std::unique_ptr<IRExpr>;

class IRBinaryExpr : public IRExpr {
public:
    IRBinaryExpr(IRExprPtr left, Token op, IRExprPtr right)
        : left(std::move(left)), op(op), right(std::move(right)) {}
    IRExprPtr left;
    Token op;
    IRExprPtr right;
};

class IRUnaryExpr : public IRExpr {
public:
    IRUnaryExpr(Token op, IRExprPtr operand)
        : op(op), operand(std::move(operand)) {}
    Token op;
    IRExprPtr operand;
};

class IRCallExpr : public IRExpr {
public:
    IRCallExpr(IRExprPtr callee, std::vector<IRExprPtr> args)
        : callee(std::move(callee)), args(std::move(args)) {}
    IRExprPtr callee;
    std::vector<IRExprPtr> args;
};

class IRIdentifierExpr : public IRExpr {
public:
    explicit IRIdentifierExpr(const std::string& name) : name(name) {}
    std::string name;
};

class IRLiteralExpr : public IRExpr {
public:
    IRLiteralExpr(std::variant<std::monostate, bool, float, int, std::string> value)
        : value(value) {}
    std::variant<std::monostate, bool, float, int, std::string> value;
};

class IRMemberExpr : public IRExpr {
public:
    IRMemberExpr(IRExprPtr object, const std::string& member)
        : object(std::move(object)), member(member) {}
    IRExprPtr object;
    std::string member;
};

class IRIndexExpr : public IRExpr {
public:
    IRIndexExpr(IRExprPtr object, IRExprPtr index)
        : object(std::move(object)), index(std::move(index)) {}
    IRExprPtr object;
    IRExprPtr index;
};

// (4) Concrete Stmt nodes — no resolvedType (only expressions have types).
class IRStmt : public IRNode {
public:
    virtual ~IRStmt() = default;
};
using IRStmtPtr = std::unique_ptr<IRStmt>;

class IRLetStmt : public IRStmt {
public:
    IRLetStmt(const std::string& name, IRExprPtr initializer)
        : name(name), initializer(std::move(initializer)) {}
    std::string name;
    IRExprPtr initializer;
};

class IRReturnStmt : public IRStmt {
public:
    explicit IRReturnStmt(IRExprPtr value) : value(std::move(value)) {}
    IRExprPtr value;
};

class IRIfStmt : public IRStmt {
public:
    IRIfStmt() = default;
    IRIfStmt(IRExprPtr condition,
             std::vector<IRStmtPtr> thenBranch,
             std::vector<IRStmtPtr> elseBranch)
        : condition(std::move(condition)),
          thenBranch(std::move(thenBranch)),
          elseBranch(std::move(elseBranch)) {}
    IRExprPtr condition;
    std::vector<IRStmtPtr> thenBranch;
    std::vector<IRStmtPtr> elseBranch;
};

class IRForStmt : public IRStmt {
public:
    IRForStmt() = default;
    IRForStmt(const std::string& variable, IRExprPtr iterable, std::vector<IRStmtPtr> body)
        : variable(variable), iterable(std::move(iterable)), body(std::move(body)) {}
    std::string variable;
    IRExprPtr iterable;
    std::vector<IRStmtPtr> body;
};

class IRExprStmt : public IRStmt {
public:
    explicit IRExprStmt(IRExprPtr expr) : expr(std::move(expr)) {}
    IRExprPtr expr;
};

class IRVariantAttribute : public IRStmt {
public:
    explicit IRVariantAttribute(const std::string& name) : name(name) {}
    std::string name;
};

// (5) Declaration node (discriminated wrapper for Uniform / Property /
// Texture). The IR unifies these three AST classes into a single tagged
// struct because they share the same emission pattern in backends
// (emit-as-uniform-decl, emit-as-uniform-with-init, emit-as-sampler)
// and the discriminator simplifies backend dispatch.
//
// Why unify now (Phase 3.1)?
// - The AST's three classes leak a GLSL lexeme string in UniformDecl
//   (`type: "float"`, `"vec3"`, ...) and have no sampler-kind on
//   TextureDecl. The IR fixes both: `uniformType` is a Type pointer
//   (target-agnostic) and `samplerKind` is an enum.
// - The backends (BGFX today, HLSL/WGSL in 3.2/3.3) iterate one vector
//   of declarations per material rather than three sequential scans.
class IRDeclaration : public IRNode {
public:
    // Storage buffer access. GLSL's `buffer Name { ... } Name;` block is
    // used for both read and read-write forms — qualifiers live on the
    // type, not the block. The access field is therefore preserved in
    // IR mainly for the future HLSL emitter (Phase 5+), which maps
    // Read → `StructuredBuffer<T>` and ReadWrite → `RWStructuredBuffer<T>`.
    enum class StorageAccess : uint8_t { Read, ReadWrite };

    enum class Kind : uint8_t { Uniform, Property, Texture, Storage, Shared, UniformBlock };

    Kind kind;
    std::string name;

    // === Uniform ===
    std::shared_ptr<Type> uniformType;     // non-null only when kind == Uniform
    IRExprPtr uniformInit;                 // optional initializer (some uniforms default-init)
    // Fixed-size array length for material-body `uniform T name[N]`.
    // 0 = non-array; >0 ⇒ emit `uniform T name[N]` + createUniform count=N.
    int uniformArrayLength = 0;

    // === Property ===
    IRExprPtr propertyInit;                // non-null only when kind == Property

    // === Texture ===
    SamplerKind samplerKind = SamplerKind::Sampler2D;  // only when kind == Texture
    int binding = -1;                                   // optional; backends may override

    // === Storage (Phase 3.2 Block 3 + Phase 3.5-A) ===
    std::shared_ptr<Type> storageElementType;     // non-null only when kind == Storage
    StorageAccess storageAccess = StorageAccess::Read;  // only when kind == Storage
    // Phase 3.5-A: optional explicit GLSL binding slot. -1 = no
    // explicit binding (the BGFX backend auto-assigns at emit time,
    // starting from 0 and skipping any explicit slot). >= 0 = user
    // wrote `storage X : rwstructuredbuffer<T> binding N;` and the
    // IRGenerator propagated the literal here. The BGFX backend emits
    // `layout(std430, binding = N)` when >= 0 and detects duplicate
    // bindings at compile time. The HLSL backend (Phase 5+) maps
    // this to `register(t[N])`; the WGSL backend maps to
    // `@group(0) @binding(N)`.
    int storageBinding = -1;                     // only when kind == Storage

    // === Shared (Phase 3.3 Block 4) ===
    // Workgroup-shared local memory. `shared T name[N];` in GLSL.
    // All threads in the same workgroup see the same memory; the
    // array size is fixed at compile time. Element type is a builtin
    // scalar / vector (float / int / uint / vec3 / etc.).
    std::shared_ptr<Type> sharedElementType;      // non-null only when kind == Shared
    int sharedSize = 0;                          // only when kind == Shared

    // === UniformBlock (Phase 3.4 + 3.5-B) ===
    // Top-level UBO. Field vectors mirror the AST field order (so
    // backends can emit `T0 f0; T1 f1;` byte-equal).
    //
    // `uboBinding` (Phase 3.5-B): optional explicit GLSL `binding = N`
    // slot. -1 means "no explicit binding — backend auto-assigns at
    // emit time" (Phase 3.4 historical behavior); >= 0 means the user
    // wrote `uniformblock X { ... } binding N;` and the IRGenerator
    // propagated the literal here. The BGFX backend always emits
    // `layout(std140, binding = N)` — the auto-assignment at emit
    // time allocates slots starting from max(explicit) + 1.
    //
    // Server-side std140 sizeof/alignment is deferred to Phase 5+ —
    // Phase 3.4 trusts the GLSL compiler to compute the layout.
    std::vector<std::shared_ptr<Type>> uboFields;     // only when kind == UniformBlock
    std::vector<std::string>          uboFieldNames;  // parallel to uboFields
    // Phase 1 RD-04: parallel array-length vector (0 = non-array,
    // >0 = `type name[N]` source form). Std140 layout and BGFX emit
    // use this to expand into N*elementSize bytes / `mat4 bones[N];`.
    std::vector<int>                  uboFieldArrayLengths;
    int uboBinding = -1;                              // only when kind == UniformBlock; Phase 3.5-B semantics

    IRDeclaration() : kind(Kind::Uniform) {}
};

// (6) ShaderParam — same shape as AST but living in the IR namespace.
// Carries PhoskiaSemantic so each backend does its own slot mapping
// (bgfx: POSITION/NORMAL/COLOR0/TEXCOORD0; HLSL: SV_Position/SV_Normal/etc;
// WGSL: @builtin(position) / @location(0) etc).
class IRShaderParam : public IRStmt {
public:
    enum class Direction { In, Out };
    Direction dir;
    std::string name;
    PhoskiaSemantic semantic;
    IRExprPtr defaultValue;  // only meaningful for `out`
};

// (7) Vertex/Fragment/Compute/Function decls — same shape as AST.
class IRVertexFunc : public IRStmt {
public:
    IRVertexFunc() = default;
    IRVertexFunc(std::vector<IRStmtPtr> params, std::vector<IRStmtPtr> body)
        : params(std::move(params)), body(std::move(body)) {}
    std::vector<IRStmtPtr> params;  // contains IRShaderParam entries
    std::vector<IRStmtPtr> body;    // contains IRLetStmt / IRReturnStmt / IRExprStmt / IRVariantAttribute
};

class IRFragmentFunc : public IRStmt {
public:
    IRFragmentFunc() = default;
    IRFragmentFunc(std::vector<IRStmtPtr> inputs, std::vector<IRStmtPtr> body)
        : inputs(std::move(inputs)), body(std::move(body)) {}
    IRFragmentFunc(std::vector<IRStmtPtr> inputs,
                   std::vector<IRStmtPtr> outputs,
                   std::vector<IRStmtPtr> body)
        : inputs(std::move(inputs))
        , outputs(std::move(outputs))
        , body(std::move(body)) {}
    std::vector<IRStmtPtr> inputs;   // IRShaderParam (In)
    std::vector<IRStmtPtr> outputs;  // IRShaderParam (Out) — MRT targets
    std::vector<IRStmtPtr> body;
};

// Material: contains declarations (uniforms / properties / textures) plus
// a single vertex function and a single fragment function. The IR separates
// the four decl kinds into pre-sorted fields so backends don't have to
// scan a single vector twice.
class IRMaterialDecl : public IRStmt {
public:
    std::string name;
    std::vector<std::unique_ptr<IRDeclaration>> declarations;  // Uniform / Property / Texture
    std::unique_ptr<IRVertexFunc> vertex;
    std::unique_ptr<IRFragmentFunc> fragment;
};

// Compute: GPGPU kernel. Phase 3.2 Block 3 adds storage buffer
// declarations alongside the existing body (thread_id / group_id /
// dispatch_id are 0-arg builtin calls handled inline by the BGFX
// backend — no IR change needed for them). The IR mirrors the
// IRMaterialDecl shape: a pre-sorted `declarations` vector plus the
// body. Phase 3.2 only stores Storage-kind declarations in
// `declarations`; compute uniforms / properties land here too if they
// ever become a thing, but Phase 3.2 doesn't expose them.
//
// Phase 3.3 Block 2: optional `[numthreads(X, Y, Z)]` attribute.
// `hasNumThreads` distinguishes "user wrote the attribute" from "user
// didn't specify" (the latter falls back to the BGFX backend's
// hardcoded 64 default). When false, numThreads is uninitialised —
// check hasNumThreads before reading.
class IRComputeDecl : public IRStmt {
public:
    std::string name;
    std::vector<std::unique_ptr<IRDeclaration>> declarations;  // Storage (Phase 3.2)
    std::vector<IRStmtPtr> body;
    bool hasNumThreads = false;
    std::array<uint32_t, 3> numThreads{};
};

// (8) Top-level IR container.
struct IRProgram {
    std::vector<std::unique_ptr<IRMaterialDecl>> materials;
    std::vector<std::unique_ptr<IRComputeDecl>> computes;
    // Phase 3.4: top-level uniform buffer objects. Each entry is an
    // IRDeclaration with kind=UniformBlock. The IRGenerator walks
    // these once at conversion time to emit `layout(std140, binding = N)
    // uniform Name { ... } Name;` lines into every shader stage that
    // uses the block (Phase 3.4: emit into every vs/fs/cs — GLSL
    // allows the same block in multiple stages).
    std::vector<std::unique_ptr<IRDeclaration>> uniformBlocks;
    // Non-fatal diagnostics from IR generation (e.g. "could not infer
    // type for X; defaulting to vec4"). Backends can ignore these;
    // callers can surface them via CompileResult.
    std::vector<std::string> warnings;
};

// (9) IRGenerator — single entry point that lowers a parsed Phoskia
// Program (AST) into an IRProgram. Optionally takes a pre-populated
// TypeEnvironment from SemanticAnalyzer; if absent, it runs TypeInference
// on demand (graceful degradation).
class IRGenerator {
public:
    IRProgram generate(const phoskia::Program& ast,
                       std::shared_ptr<phoskia::TypeEnvironment> typeEnv = nullptr);

private:
    // Lowering helpers — one per AST node class. Mirrors the AST shape;
    // no big-picture transformations (those are backend work / future SSA).
    std::unique_ptr<IRExpr> lowerExpr(const phoskia::Expr& e,
                                      phoskia::TypeEnvironment* scopeEnv = nullptr);
    std::unique_ptr<IRStmt> lowerStmt(const phoskia::Stmt& s,
                                      phoskia::TypeEnvironment* scopeEnv = nullptr);
    std::unique_ptr<IRDeclaration> lowerDecl(const phoskia::Stmt& s);

    // Helpers for shader-param + vertex/fragment decl lowering. These
    // extract pre-sorted IR fields directly into the IRMaterialDecl.
    std::unique_ptr<IRShaderParam> lowerShaderParam(const phoskia::ShaderParam& p,
                                                  phoskia::TypeEnvironment* scopeEnv = nullptr);
    std::unique_ptr<IRVertexFunc> lowerVertexFuncWithEnv(const phoskia::VertexFunc& vf,
                                                         phoskia::TypeEnvironment& env);
    std::unique_ptr<IRFragmentFunc> lowerFragmentFuncWithEnv(const phoskia::FragmentFunc& ff,
                                                             phoskia::TypeEnvironment& env);
    std::unique_ptr<IRMaterialDecl> lowerMaterialDecl(const phoskia::MaterialDecl& m);
    std::unique_ptr<IRComputeDecl> lowerComputeDecl(const phoskia::ComputeDecl& c);

    // Program-scope UniformBlock instance types (StructType) — seeded into
    // every material vs/fs env so `Lights.dirs[i].xyz` resolves to vec3
    // instead of a free TypeVar (which the BGFX emitter prints as float
    // and silently breaks Lambert NdotL on D3D).
    std::vector<std::pair<std::string, std::shared_ptr<Type>>> _programUboTypes;

    // Resolve type for a single expression. Runs TypeInference when no
    // pre-resolved type is available; returns nullptr when even
    // TypeInference can't infer a concrete type. `scopeEnv` (when
    // non-null) is the per-block env that has accumulated uniform +
    // property + in/out + let-stmt bindings — passing it ensures
    // identifier references resolve to concrete types instead of
    // fresh TypeVars (critical for chain-of-arithmetic on prior lets).
    std::shared_ptr<Type> resolveType(const phoskia::Expr& e,
                                      phoskia::TypeEnvironment* scopeEnv = nullptr);

    // Resolve a TypeVar chain to a concrete Type (walks solution ptrs).
    std::shared_ptr<Type> resolveTypeVar(std::shared_ptr<Type> t) const;

    // Convert a Phoskia AST type-pointer (or GLSL lexeme) to a Type
    // pointer. Used for uniform type bindings.
    std::shared_ptr<Type> typeFromGLSLLexeme(const std::string& lex) const;

    // Warnings accumulated during generation.
    std::vector<std::string> _warnings;

    // (Phase 3.4 had a `nextBinding_` UBO binding slot counter here.
    // Phase 3.5-B removed it: binding slot resolution is now the
    // backend's responsibility (mirroring how SSBO storageBinding
    // resolves at BGFX emit time, not IRGen time). The IRGenerator
    // is purely declarative — it propagates the AST's `binding`
    // field (-1 or literal) verbatim into `IRDeclaration::uboBinding`.)

    // Caller-supplied TypeEnvironment (from SemanticAnalyzer); nullptr
    // means "no env — fall back to running TypeInference per expression".
    std::shared_ptr<phoskia::TypeEnvironment> _env;
};

} // namespace ayt::shader::phoskia::ir