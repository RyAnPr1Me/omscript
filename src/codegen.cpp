#include "codegen.h"
#include "diagnostic.h"
#include "egraph.h"
#include "opt_context.h"
#include "opt_orchestrator.h"
#include "rlc_pass.h"
#include "synthesize.h"
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <llvm/ADT/StringMap.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/ConstantRange.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/PGOOptions.h>
#include <llvm/Support/TargetSelect.h>
#if LLVM_VERSION_MAJOR < 22
#include <llvm/Support/VirtualFileSystem.h>
#endif
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/LoopDistribute.h>
#include <llvm/Transforms/Utils.h>
#include <optional>
#include <stdexcept>

// LLVM 21 removed Attribute::NoCapture in favour of the captures(...) attribute.
#if LLVM_VERSION_MAJOR >= 21
#define OMSC_ADD_NOCAPTURE(fn, idx)                                                                                    \
    (fn)->addParamAttr((idx), llvm::Attribute::getWithCaptureInfo((fn)->getContext(), llvm::CaptureInfo::none()))
#else
#define OMSC_ADD_NOCAPTURE(fn, idx) (fn)->addParamAttr((idx), llvm::Attribute::NoCapture)
#endif

// LLVM 19 introduced getOrInsertDeclaration; older versions only have getDeclaration.
#if LLVM_VERSION_MAJOR >= 19
#define OMSC_GET_INTRINSIC llvm::Intrinsic::getOrInsertDeclaration
#else
#define OMSC_GET_INTRINSIC llvm::Intrinsic::getDeclaration
#endif

namespace {

using omscript::ArrayExpr;
using omscript::AssignExpr;
using omscript::ASTNodeType;
using omscript::BinaryExpr;
using omscript::BlockStmt;
using omscript::CallExpr;
using omscript::DoWhileStmt;
using omscript::Expression;
using omscript::ExprStmt;
using omscript::ForEachStmt;
using omscript::ForStmt;
using omscript::IdentifierExpr;
using omscript::IfStmt;
using omscript::IndexAssignExpr;
using omscript::IndexExpr;
using omscript::LiteralExpr;
using omscript::PostfixExpr;
using omscript::PrefixExpr;
using omscript::ReturnStmt;
using omscript::Statement;
using omscript::TernaryExpr;
using omscript::UnaryExpr;
using omscript::VarDecl;
using omscript::WhileStmt;

/// Concurrency builtin names.  User functions that call any of these must NOT
static const std::unordered_set<std::string> kConcurrencyBuiltins = {"thread_create", "thread_join",  "mutex_new",
                                                                     "mutex_lock",    "mutex_unlock", "mutex_destroy"};

/// Recursively check if an expression tree contains a call to any name in @p names.
static bool exprCallsAny(const Expression* e, const std::unordered_set<std::string>& names) {
    if (!e)
        return false;
    switch (e->type) {
    case ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<const CallExpr*>(e);
        if (names.count(call->callee))
            return true;
        for (auto& arg : call->arguments)
            if (exprCallsAny(arg.get(), names))
                return true;
        return false;
    }
    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<const BinaryExpr*>(e);
        return exprCallsAny(bin->left.get(), names) || exprCallsAny(bin->right.get(), names);
    }
    case ASTNodeType::UNARY_EXPR:
        return exprCallsAny(static_cast<const UnaryExpr*>(e)->operand.get(), names);
    case ASTNodeType::TERNARY_EXPR: {
        auto* tern = static_cast<const TernaryExpr*>(e);
        return exprCallsAny(tern->condition.get(), names) || exprCallsAny(tern->thenExpr.get(), names) ||
               exprCallsAny(tern->elseExpr.get(), names);
    }
    case ASTNodeType::INDEX_EXPR: {
        auto* idx = static_cast<const IndexExpr*>(e);
        return exprCallsAny(idx->array.get(), names) || exprCallsAny(idx->index.get(), names);
    }
    case ASTNodeType::INDEX_ASSIGN_EXPR: {
        auto* ia = static_cast<const IndexAssignExpr*>(e);
        return exprCallsAny(ia->array.get(), names) || exprCallsAny(ia->index.get(), names) ||
               exprCallsAny(ia->value.get(), names);
    }
    case ASTNodeType::ASSIGN_EXPR:
        return exprCallsAny(static_cast<const AssignExpr*>(e)->value.get(), names);
    default:
        return false;
    }
}

/// Recursively check if a statement tree contains a call to any name in @p names.
static bool stmtCallsAny(const Statement* s, const std::unordered_set<std::string>& names) {
    if (!s)
        return false;
    switch (s->type) {
    case ASTNodeType::BLOCK: {
        auto* blk = static_cast<const BlockStmt*>(s);
        for (auto& st : blk->statements)
            if (stmtCallsAny(st.get(), names))
                return true;
        return false;
    }
    case ASTNodeType::RETURN_STMT:
        return exprCallsAny(static_cast<const omscript::ReturnStmt*>(s)->value.get(), names);
    case ASTNodeType::EXPR_STMT:
        return exprCallsAny(static_cast<const ExprStmt*>(s)->expression.get(), names);
    case ASTNodeType::VAR_DECL:
        return exprCallsAny(static_cast<const VarDecl*>(s)->initializer.get(), names);
    case ASTNodeType::IF_STMT: {
        auto* ifS = static_cast<const IfStmt*>(s);
        return exprCallsAny(ifS->condition.get(), names) || stmtCallsAny(ifS->thenBranch.get(), names) ||
               stmtCallsAny(ifS->elseBranch.get(), names);
    }
    case ASTNodeType::WHILE_STMT: {
        auto* wh = static_cast<const WhileStmt*>(s);
        return exprCallsAny(wh->condition.get(), names) || stmtCallsAny(wh->body.get(), names);
    }
    case ASTNodeType::DO_WHILE_STMT: {
        auto* dw = static_cast<const DoWhileStmt*>(s);
        return stmtCallsAny(dw->body.get(), names) || exprCallsAny(dw->condition.get(), names);
    }
    case ASTNodeType::FOR_STMT: {
        auto* fr = static_cast<const ForStmt*>(s);
        return exprCallsAny(fr->start.get(), names) || exprCallsAny(fr->end.get(), names) ||
               exprCallsAny(fr->step.get(), names) || stmtCallsAny(fr->body.get(), names);
    }
    case ASTNodeType::FOR_EACH_STMT: {
        auto* fe = static_cast<const ForEachStmt*>(s);
        return exprCallsAny(fe->collection.get(), names) || stmtCallsAny(fe->body.get(), names);
    }
    default:
        return false;
    }
}

/// Returns true if the function body contains calls to any concurrency builtin.
static bool usesConcurrencyPrimitive(const omscript::FunctionDecl* func) {
    if (!func->body)
        return false;
    return stmtCallsAny(func->body.get(), kConcurrencyBuiltins);
}


} // namespace

namespace omscript {

// File-scope SIMD type registry — single source of truth for the
struct SimdTypeRow {
    const char* name;   // OmScript annotation, e.g. "i32x8"
    unsigned bits;      // element bit width (8, 16, 32, 64)
    bool isFloat;       // true → IEEE float of `bits`; false → integer
    unsigned lanes;     // lane count
};

static constexpr SimdTypeRow kSimdTypeRegistry[] = {
    // f32 lanes — SSE / AVX / AVX-512
    {"f32x4",  32, true,  4 },
    {"f32x8",  32, true,  8 },
    {"f32x16", 32, true,  16},
    // f64 lanes — SSE2 / AVX / AVX-512
    {"f64x2",  64, true,  2 },
    {"f64x4",  64, true,  4 },
    {"f64x8",  64, true,  8 },
    // 8-bit integer lanes — SSE2 / AVX2
    {"i8x16",   8, false, 16},
    {"u8x16",   8, false, 16},
    {"i8x32",   8, false, 32},
    {"u8x32",   8, false, 32},
    // 16-bit integer lanes — SSE2 / AVX2
    {"i16x8",  16, false, 8 },
    {"u16x8",  16, false, 8 },
    {"i16x16", 16, false, 16},
    {"u16x16", 16, false, 16},
    // 32-bit integer lanes — SSE2 / AVX2 / AVX-512
    {"i32x4",  32, false, 4 },
    {"u32x4",  32, false, 4 },
    {"i32x8",  32, false, 8 },
    {"u32x8",  32, false, 8 },
    {"i32x16", 32, false, 16},
    {"u32x16", 32, false, 16},
    // 64-bit integer lanes — SSE2 / AVX2 / AVX-512
    {"i64x2",  64, false, 2 },
    {"u64x2",  64, false, 2 },
    {"i64x4",  64, false, 4 },
    {"u64x4",  64, false, 4 },
    {"i64x8",  64, false, 8 },
    {"u64x8",  64, false, 8 },
};

/// Returns true if a type-annotation string represents an unsigned integer
/// (uint, or any uN for N in [1..256]).
static bool isUnsignedAnnotation(const std::string& tn) {
    if (tn == "uint") return true;
    if (tn.size() >= 2 && tn[0] == 'u') {
        for (size_t j = 1; j < tn.size(); ++j)
            if (!std::isdigit(static_cast<unsigned char>(tn[j]))) return false;
        return true; // u1, u2, ..., u256, u64, etc.
    }
    return false;
}

// Canonical set of all stdlib built-in function names.
// These functions are always compiled to native machine code via LLVM IR.
static const std::unordered_set<std::string> stdlibFunctions = {"abs",
                                                                "acos",
                                                                "array_any",
                                                                "array_concat",
                                                                "array_contains",
                                                                "array_copy",
                                                                "array_count",
                                                                "array_drop",
                                                                "array_every",
                                                                "array_fill",
                                                                "array_filter",
                                                                "array_find",
                                                                "array_insert",
                                                                "array_last",
                                                                "array_map",
                                                                "array_max",
                                                                "array_mean",
                                                                "array_min",
                                                                "array_product",
                                                                "array_reduce",
                                                                "array_remove",
                                                                "array_rotate",
                                                                "array_slice",
                                                                "array_take",
                                                                "array_unique",
                                                                "asin",
                                                                "assert",
                                                                "assume",
                                                                "alloc",
                                                                "atan",
                                                                "atan2",
                                                                "bitreverse",
                                                                "bswap",
                                                                "cbrt",
                                                                "ceil",
                                                                "char_at",
                                                                "char_code",
                                                                "clamp",
                                                                "clz",
                                                                "command",
                                                                "copysign",
                                                                "cos",
                                                                "ctz",
                                                                "exit_program",
                                                                "exit",
                                                                "exp",
                                                                "exp2",
                                                                "expect",
                                                                "fast_add",
                                                                "fast_div",
                                                                "fast_mul",
                                                                "fast_sub",
                                                                "file_append",
                                                                "file_exists",
                                                                "file_read",
                                                                "file_write",
                                                                "filter",
                                                                "floor",
                                                                "fma",
                                                                "gcd",
                                                                "hypot",
                                                                "index_of",
                                                                "input",
                                                                "input_line",
                                                                "is_alnum",
                                                                "is_alpha",
                                                                "is_digit",
                                                                "is_even",
                                                                "is_lower",
                                                                "is_odd",
                                                                "is_power_of_2",
                                                                "is_space",
                                                                "is_upper",
                                                                "lcm",
                                                                "len",
                                                                "log",
                                                                "log10",
                                                                "log2",
                                                                "map_filter",
                                                                "map_get",
                                                                "map_has",
                                                                "map_invert",
                                                                "map_keys",
                                                                "map_merge",
                                                                "map_new",
                                                                "map_remove",
                                                                "map_set",
                                                                "map_size",
                                                                "map_values",
                                                                "max",
                                                                "max_float",
                                                                "min",
                                                                "min_float",
                                                                "number_to_string",
                                                                "newRegion",
                                                                "pop",
                                                                "popcount",
                                                                "pow",
                                                                "precise_add",
                                                                "precise_div",
                                                                "precise_mul",
                                                                "precise_sub",
                                                                "print",
                                                                "print_char",
                                                                "println",
                                                                "push",
                                                                "random",
                                                                "range",
                                                                "range_step",
                                                                "reverse",
                                                                "rotate_left",
                                                                "rotate_right",
                                                                "round",
                                                                "saturating_add",
                                                                "saturating_sub",
                                                                "shell",
                                                                "sign",
                                                                "sin",
                                                                "sleep",
                                                                "sort",
                                                                "sqrt",
                                                                "str_chars",
                                                                "str_concat",
                                                                "str_contains",
                                                                "str_count",
                                                                "str_ends_with",
                                                                "str_eq",
                                                                "str_filter",
                                                                "str_find",
                                                                "str_index_of",
                                                                "str_join",
                                                                "str_len",
                                                                "str_lower",
                                                                "str_lstrip",
                                                                "str_pad_left",
                                                                "str_pad_right",
                                                                "str_remove",
                                                                "str_repeat",
                                                                "str_replace",
                                                                "str_reverse",
                                                                "str_rstrip",
                                                                "str_split",
                                                                "str_starts_with",
                                                                "str_substr",
                                                                "str_to_float",
                                                                "str_to_int",
                                                                "str_trim",
                                                                "str_upper",
                                                                "string_to_number",
                                                                "sudo_command",
                                                                "env_get",
                                                                "env_set",
                                                                "sum",
                                                                "swap",
                                                                "tan",
                                                                "time",
                                                                "to_char",
                                                                "to_float",
                                                                "to_int",
                                                                "to_string",
                                                                "thread_create",
                                                                "thread_join",
                                                                "mutex_new",
                                                                "mutex_lock",
                                                                "mutex_unlock",
                                                                "mutex_destroy",
                                                                "typeof",
                                                                "unreachable",
                                                                "write",
                                                                // Numeric type-cast functions: i32(x), u8(x), etc.
                                                                // These are parsed as CallExpr nodes and handled
                                                                // as builtins; they must be whitelisted so that
                                                                // OPTMAX functions can call them without error.
                                                                "i8",  "u8",
                                                                "i16", "u16",
                                                                "i32", "u32",
                                                                "i64", "u64",
                                                                "f32", "f64",
                                                                "to_i8",  "to_u8",
                                                                "to_i16", "to_u16",
                                                                "to_i32", "to_u32",
                                                                "to_i64", "to_u64",
                                                                "to_f32", "to_f64"};

bool isStdlibFunction(const std::string& name) {
    return stdlibFunctions.find(name) != stdlibFunctions.end();
}

CodeGenerator::CodeGenerator(OptimizationLevel optLevel)
    : inOptMaxFunction(false), hasOptMaxFunctions(false), optimizationLevel(optLevel) {
    context = std::make_unique<llvm::LLVMContext>();
    module = std::make_unique<llvm::Module>("omscript", *context);
    builder = std::make_unique<llvm::IRBuilder<>>(*context);

    setupPrintfDeclaration();
    initTBAAMetadata();
}

CodeGenerator::~CodeGenerator() = default;

// ---------------------------------------------------------------------------

void CodeGenerator::initTBAAMetadata() {
    // Build a TBAA type hierarchy so LLVM knows that different memory regions
    auto& C = *context;
    tbaaRoot_ = llvm::MDNode::get(C, {llvm::MDString::get(C, "OmScript TBAA")});

    auto makeTBAAType = [&](const char* name) -> llvm::MDNode* {
        return llvm::MDNode::get(C, {
            llvm::MDString::get(C, name),
            tbaaRoot_,
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                llvm::Type::getInt64Ty(C), 0))
        });
    };

    llvm::MDNode* tbaaLenType    = makeTBAAType("array length");
    llvm::MDNode* tbaaElemType   = makeTBAAType("array element");
    llvm::MDNode* tbaaStructType = makeTBAAType("struct field");
    tbaaStructTypeNode_ = tbaaStructType;
    llvm::MDNode* tbaaStrType    = makeTBAAType("string data");
    llvm::MDNode* tbaaMapKeyType = makeTBAAType("map key");
    llvm::MDNode* tbaaMapValType = makeTBAAType("map value");
    llvm::MDNode* tbaaMapHashType = makeTBAAType("map hash");
    llvm::MDNode* tbaaMapMetaType = makeTBAAType("map meta");

    // Access tag nodes: !{ !base-type, !access-type, offset }
    // For scalar types, base == access.
    auto* zero = llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(C), 0));
    tbaaArrayLen_    = llvm::MDNode::get(C, {tbaaLenType, tbaaLenType, zero});
    tbaaArrayElem_   = llvm::MDNode::get(C, {tbaaElemType, tbaaElemType, zero});
    tbaaStructField_ = llvm::MDNode::get(C, {tbaaStructType, tbaaStructType, zero});
    tbaaStringData_  = llvm::MDNode::get(C, {tbaaStrType, tbaaStrType, zero});
    tbaaMapKey_      = llvm::MDNode::get(C, {tbaaMapKeyType, tbaaMapKeyType, zero});
    tbaaMapVal_      = llvm::MDNode::get(C, {tbaaMapValType, tbaaMapValType, zero});
    tbaaMapHash_     = llvm::MDNode::get(C, {tbaaMapHashType, tbaaMapHashType, zero});
    tbaaMapMeta_     = llvm::MDNode::get(C, {tbaaMapMetaType, tbaaMapMetaType, zero});

    // !range metadata: array lengths are always in [0, INT64_MAX).
    auto* i64Ty = llvm::Type::getInt64Ty(C);
    arrayLenRangeMD_ = llvm::MDNode::get(C, {
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 0)),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
            i64Ty, static_cast<uint64_t>(INT64_MAX)))
    });
    // !range [0, 2): boolean i64 results (is_alpha, is_digit, str_eq, etc.)
    boolRangeMD_ = llvm::MDNode::get(C, {
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 0)),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 2))
    });
    // !range [0, 256): char i64 results (char_at)
    charRangeMD_ = llvm::MDNode::get(C, {
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 0)),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 256))
    });
    // !range [0, 65): bit-count i64 results (popcount/clz/ctz).
    bitcountRangeMD_ = llvm::MDNode::get(C, {
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 0)),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 65))
    });
}

llvm::MDNode* CodeGenerator::getOrCreateFieldTBAA(const std::string& structType, size_t fieldIdx) {
    auto key = std::make_pair(structType, fieldIdx);
    auto it = tbaaStructFieldCache_.find(key);
    if (it != tbaaStructFieldCache_.end()) return it->second;

    // Build a unique per-field TBAA type node as a child of tbaaStructTypeNode_.
    auto& C = *context;
    auto* zero = llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(C), 0));
    const std::string nodeName = "struct." + structType + "." + std::to_string(fieldIdx);
    llvm::MDNode* fieldTypeNode = llvm::MDNode::get(C, {
        llvm::MDString::get(C, nodeName),
        tbaaStructTypeNode_,
        zero
    });
    // Access tag: {fieldTypeNode, fieldTypeNode, 0}
    llvm::MDNode* accessTag = llvm::MDNode::get(C, {fieldTypeNode, fieldTypeNode, zero});
    tbaaStructFieldCache_[key] = accessTag;
    return accessTag;
}

void CodeGenerator::setupPrintfDeclaration() {
    // Declare printf function for output
    std::vector<llvm::Type*> printfArgs;
    printfArgs.push_back(llvm::PointerType::getUnqual(*context));

    llvm::FunctionType* printfType = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), printfArgs, true);

    auto* fn = llvm::Function::Create(printfType, llvm::Function::ExternalLinkage, "printf", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    // printf reads through its format string arg (nocapture, readonly).
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 0);
}

llvm::Function* CodeGenerator::getPrintfFunction() {
    return module->getFunction("printf");
}

llvm::Type* CodeGenerator::getDefaultType() {
    // Default to 64-bit integer for dynamic typing
    return llvm::Type::getInt64Ty(*context);
}

llvm::Type* CodeGenerator::getFloatType() {
    return llvm::Type::getDoubleTy(*context);
}

llvm::Type* CodeGenerator::resolveAnnotatedType(const std::string& annotation) {
    // Strip reference prefix '&' — borrowed references share the same
    // underlying type (e.g., &i32 → i32).
    std::string ann = annotation;
    if (!ann.empty() && ann[0] == '&') {
        ann = ann.substr(1);
    }
    if (ann == "ptr" || (ann.rfind("ptr<", 0) == 0 && ann.back() == '>'))
        return llvm::PointerType::getUnqual(*context);
    if (ann == "float" || ann == "double" || ann == "f64" || ann == "float64")
        return getFloatType();                                  // f64 (double)
    if (ann == "f32" || ann == "float32")
        return llvm::Type::getFloatTy(*context);                // f32 (single)
    if (ann == "bool")
        return llvm::Type::getInt1Ty(*context);                 // i1
    if (ann == "i8" || ann == "u8")
        return llvm::Type::getInt8Ty(*context);                 // i8/u8
    if (ann == "i16" || ann == "u16")
        return llvm::Type::getInt16Ty(*context);                // i16/u16
    if (ann == "i32" || ann == "u32")
        return llvm::Type::getInt32Ty(*context);                // i32/u32
    // -----------------------------------------------------------------------
    for (const SimdTypeRow& r : kSimdTypeRegistry) {
        if (ann == r.name) {
            llvm::Type* elemTy =
                r.isFloat
                    ? (r.bits == 32 ? llvm::Type::getFloatTy(*context)
                                    : llvm::Type::getDoubleTy(*context))
                    : llvm::Type::getIntNTy(*context, r.bits);
            return llvm::FixedVectorType::get(elemTy, r.lanes);
        }
    }
    // -----------------------------------------------------------------------
    if (ann == "string")
        return llvm::PointerType::getUnqual(*context);
    // bigint: heap-allocated arbitrary-precision integer — opaque pointer
    if (ann == "bigint")
        return llvm::PointerType::getUnqual(*context);
    // Array types: int[], string[], float[], etc.
    if (ann.size() >= 2 && ann.compare(ann.size() - 2, 2, "[]") == 0)
        return llvm::PointerType::getUnqual(*context);
    // Dict/map types
    if (ann == "dict" || ann.rfind("dict[", 0) == 0)
        return llvm::PointerType::getUnqual(*context);
    // Known struct types — heap-allocated, accessed via pointer
    if (structDefs_.count(ann))
        return llvm::PointerType::getUnqual(*context);

    // General iN/uN handler for arbitrary bit widths [1..256].
    {
        const std::string& a = ann;
        if (a.size() >= 2 && (a[0] == 'i' || a[0] == 'u')) {
            bool allDigits = true;
            int n = 0;
            for (size_t j = 1; j < a.size(); ++j) {
                if (!std::isdigit(static_cast<unsigned char>(a[j]))) { allDigits = false; break; }
                n = n * 10 + (a[j] - '0');
                if (n > 256) { allDigits = false; break; }
            }
            if (allDigits && n >= 1 && n <= 256) {
                return llvm::IntegerType::get(*context, static_cast<unsigned>(n));
            }
        }
    }

    // "int", "uint", generics, and empty annotations map to i64.
    return getDefaultType();
}

llvm::StructType* CodeGenerator::getOrCreateStructLLVMType(const std::string& name) {
    auto cached = structLLVMTypes_.find(name);
    if (cached != structLLVMTypes_.end())
        return cached->second;

    // Look up declared field metadata.  Prefer the rich StructField list
    auto declIt = structFieldDecls_.find(name);
    auto namesIt = structDefs_.find(name);

    std::vector<llvm::Type*> elemTypes;
    if (declIt != structFieldDecls_.end() && !declIt->second.empty()) {
        elemTypes.reserve(declIt->second.size());
        for (const auto& fd : declIt->second) {
            llvm::Type* t = fd.typeName.empty() ? getDefaultType()
                                                : resolveAnnotatedType(fd.typeName);
            // bool fields would otherwise become i1, which has weird ABI
            if (t->isIntegerTy(1))
                t = llvm::Type::getInt8Ty(*context);
            elemTypes.push_back(t);
        }
    } else if (namesIt != structDefs_.end()) {
        elemTypes.assign(namesIt->second.size(), getDefaultType());
    } else {
        return nullptr; // Unknown struct
    }

    // Use a non-packed, named struct so LLVM applies the target DataLayout's
    llvm::StructType* sty =
        llvm::StructType::create(*context, elemTypes, "omsc.struct." + name, /*isPacked=*/false);
    structLLVMTypes_[name] = sty;
    return sty;
}

llvm::Value* CodeGenerator::liftFieldLoad(llvm::Value* v, const std::string& annot) {
    llvm::Type* ty = v->getType();
    // Integers narrower than the default expression width get extended so the
    if (ty->isIntegerTy()) {
        const unsigned bits = ty->getIntegerBitWidth();
        if (bits < 64) {
            const bool isUnsigned = isUnsignedAnnot(annot);
            return isUnsigned
                       ? builder->CreateZExt(v, getDefaultType(), "field.zext")
                       : builder->CreateSExt(v, getDefaultType(), "field.sext");
        }
        return v;
    }
    if (ty->isFloatTy())
        return builder->CreateFPExt(v, getFloatType(), "field.fpext");
    return v;
}

llvm::Value* CodeGenerator::convertTo(llvm::Value* v, llvm::Type* targetTy) {
    if (v->getType() == targetTy)
        return v;
    // float → int
    if (v->getType()->isDoubleTy() && targetTy->isIntegerTy())
        return builder->CreateFPToSI(v, targetTy, "ftoi");
    // int → float
    if (v->getType()->isIntegerTy() && targetTy->isDoubleTy())
        return builder->CreateSIToFP(v, targetTy, "itof");
    // ptr → int
    if (v->getType()->isPointerTy() && targetTy->isIntegerTy())
        return builder->CreatePtrToInt(v, targetTy, "ptoi");
    // int → ptr
    if (v->getType()->isIntegerTy() && targetTy->isPointerTy())
        return builder->CreateIntToPtr(v, targetTy, "itop");
    // int → int (widening)
    if (v->getType()->isIntegerTy() && targetTy->isIntegerTy()) {
        const unsigned srcBits = v->getType()->getIntegerBitWidth();
        const unsigned dstBits = targetTy->getIntegerBitWidth();
        if (srcBits < dstBits)
            return builder->CreateSExt(v, targetTy, "sext");
        if (srcBits > dstBits)
            return builder->CreateTrunc(v, targetTy, "trunc");
    }
    // ptr → float (via int)
    if (v->getType()->isPointerTy() && targetTy->isDoubleTy()) {
        auto* intVal = builder->CreatePtrToInt(v, getDefaultType(), "ptoi");
        return builder->CreateSIToFP(intVal, targetTy, "itof");
    }
    return v;
}

llvm::Value* CodeGenerator::toBool(llvm::Value* v) {
    // When the value is already i1 (from a comparison), it IS the boolean
    if (v->getType()->isIntegerTy(1)) {
        return v;
    }
    if (v->getType()->isDoubleTy()) {
        return builder->CreateFCmpONE(v, llvm::ConstantFP::get(getFloatType(), 0.0), "tobool");
    } else if (v->getType()->isPointerTy()) {
        llvm::Value* intVal = builder->CreatePtrToInt(v, getDefaultType(), "ptrtoint");
        return builder->CreateICmpNE(intVal, llvm::ConstantInt::get(getDefaultType(), 0), "tobool");
    } else {
        return builder->CreateICmpNE(v, llvm::ConstantInt::get(v->getType(), 0, true), "tobool");
    }
}

llvm::Value* CodeGenerator::toDefaultType(llvm::Value* v) {
    if (v->getType() == getDefaultType())
        return v;
    if (v->getType()->isDoubleTy()) {
        return builder->CreateFPToSI(v, getDefaultType(), "ftoi");
    }
    if (v->getType()->isPointerTy()) {
        return builder->CreatePtrToInt(v, getDefaultType(), "ptoi");
    }
    if (v->getType()->isIntegerTy()) {
        const unsigned srcBits = v->getType()->getIntegerBitWidth();
        if (srcBits > 64) {
            // Wide integers (i65–i256): keep at native width; callers that
            // need i64 must truncate explicitly with convertTo().
            return v;
        }
        // Narrow integers (i1–i63): zero-extend to i64.
        return builder->CreateZExt(v, getDefaultType(), "zext");
    }
    return v;
}

llvm::Value* CodeGenerator::ensureFloat(llvm::Value* v) {
    if (v->getType()->isDoubleTy())
        return v;
    if (v->getType()->isIntegerTy()) {
        return builder->CreateSIToFP(v, getFloatType(), "itof");
    }
    if (v->getType()->isPointerTy()) {
        // Convert pointer to int first, then to float
        llvm::Value* intVal = builder->CreatePtrToInt(v, getDefaultType(), "ptoi");
        return builder->CreateSIToFP(intVal, getFloatType(), "itof");
    }
    return v;
}

llvm::Value* CodeGenerator::convertToVectorElement(llvm::Value* v, llvm::Type* elemTy) {
    if (v->getType() == elemTy)
        return v;
    if (elemTy->isFloatTy()) {
        if (v->getType()->isDoubleTy())
            return builder->CreateFPTrunc(v, elemTy, "elem.fptrunc");
        if (v->getType()->isIntegerTy())
            return builder->CreateSIToFP(v, elemTy, "elem.sitofp");
    } else if (elemTy->isDoubleTy()) {
        if (v->getType()->isIntegerTy())
            return builder->CreateSIToFP(v, elemTy, "elem.sitofp");
        if (v->getType()->isFloatTy())
            return builder->CreateFPExt(v, elemTy, "elem.fpext");
    } else if (elemTy->isIntegerTy()) {
        if (v->getType()->isDoubleTy() || v->getType()->isFloatTy())
            return builder->CreateFPToSI(v, elemTy, "elem.fptosi");
        if (v->getType()->isIntegerTy())
            return builder->CreateIntCast(v, elemTy, true, "elem.icast");
    }
    return v;
}

llvm::Value* CodeGenerator::splatScalarToVector(llvm::Value* scalar, llvm::Type* vecTy) {
    auto* fvt = llvm::cast<llvm::FixedVectorType>(vecTy);
    scalar = convertToVectorElement(scalar, fvt->getElementType());
    llvm::Value* undef = llvm::UndefValue::get(vecTy);
    llvm::Value* ins = builder->CreateInsertElement(undef, scalar,
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0), "splat.ins");
    const llvm::SmallVector<int, 16> mask(fvt->getNumElements(), 0);
    return builder->CreateShuffleVector(ins, mask, "splat");
}

void CodeGenerator::beginScope() {
    validateScopeStacksMatch(__func__);
    scopeStack.emplace_back();
    constScopeStack.emplace_back();
    borrowScopeStack_.emplace_back(); // new borrow scope
}

void CodeGenerator::endScope() {
    validateScopeStacksMatch(__func__);
    if (scopeStack.empty()) {
        return;
    }

    // Release all borrows introduced in this scope before restoring variables.
    if (!borrowScopeStack_.empty()) {
        for (const auto& info : borrowScopeStack_.back()) {
            releaseBorrow(info.refVar);
        }
        borrowScopeStack_.pop_back();
    }

    auto& scope = scopeStack.back();
    auto& constScope = constScopeStack.back();
    for (const auto& entry : scope) {
        if (entry.second) {
            namedValues[entry.first] = entry.second;
        } else {
            namedValues.erase(entry.first);
            varTypeAnnotations_.erase(entry.first);
        }
    }
    for (const auto& entry : constScope) {
        if (entry.second.wasPreviouslyDefined) {
            constValues[entry.first] = entry.second.previousIsConst;
        } else {
            constValues.erase(entry.first);
        }
        if (entry.second.hadPreviousIntFold) {
            constIntFolds_[entry.first] = entry.second.previousIntFold;
        } else {
            constIntFolds_.erase(entry.first);
        }
        if (entry.second.hadPreviousFloatFold) {
            constFloatFolds_[entry.first] = entry.second.previousFloatFold;
        } else {
            constFloatFolds_.erase(entry.first);
        }
        if (entry.second.hadPreviousStringFold) {
            constStringFolds_[entry.first] = entry.second.previousStringFold;
        } else {
            constStringFolds_.erase(entry.first);
        }
    }
    scopeStack.pop_back();
    constScopeStack.pop_back();
}

void CodeGenerator::bindVariable(const std::string& name, llvm::Value* value, bool isConst) {
    if (!scopeStack.empty()) {
        auto& scope = scopeStack.back();
        if (scope.find(name) == scope.end()) {
            auto existing = namedValues.find(name);
            scope[name] = existing == namedValues.end() ? nullptr : existing->second;
        }
    }
    if (!constScopeStack.empty()) {
        auto& constScope = constScopeStack.back();
        if (constScope.find(name) == constScope.end()) {
            auto existingConst = constValues.find(name);
            ConstBinding binding{};
            if (existingConst == constValues.end()) {
                binding.wasPreviouslyDefined = false;
                binding.previousIsConst = false;
            } else {
                binding.wasPreviouslyDefined = true;
                binding.previousIsConst = existingConst->second;
            }
            // Save previous constIntFolds_ entry for this variable (for scope restoration).
            auto foldIt = constIntFolds_.find(name);
            if (foldIt != constIntFolds_.end()) {
                binding.hadPreviousIntFold = true;
                binding.previousIntFold = foldIt->second;
            }
            // Save previous constFloatFolds_ entry for this variable (for scope restoration).
            auto floatFoldIt = constFloatFolds_.find(name);
            if (floatFoldIt != constFloatFolds_.end()) {
                binding.hadPreviousFloatFold = true;
                binding.previousFloatFold = floatFoldIt->second;
            }
            // Save previous constStringFolds_ entry for this variable (for scope restoration).
            auto stringFoldIt = constStringFolds_.find(name);
            if (stringFoldIt != constStringFolds_.end()) {
                binding.hadPreviousStringFold = true;
                binding.previousStringFold = stringFoldIt->second;
            }
            constScope[name] = binding;
        }
    }
    namedValues[name] = value;
    constValues[name] = isConst;
    // If the variable is being rebound (not a const), remove its int/float/string fold entries
    // since the new binding may have a different value.
    if (!isConst) {
        constIntFolds_.erase(name);
        constFloatFolds_.erase(name);
        constStringFolds_.erase(name);
    }
}

void CodeGenerator::bindVariableAnnotated(const std::string& name, llvm::Value* value,
                                          const std::string& typeAnnot, bool isConst) {
    bindVariable(name, value, isConst);
    if (!typeAnnot.empty())
        varTypeAnnotations_[name] = typeAnnot;
    else
        varTypeAnnotations_.erase(name);
}

bool CodeGenerator::isUnsignedAnnot(const std::string& annot) {
    if (annot == "uint") return true;
    if (annot.size() >= 2 && annot[0] == 'u') {
        for (size_t j = 1; j < annot.size(); ++j)
            if (!std::isdigit(static_cast<unsigned char>(annot[j]))) return false;
        return true;
    }
    return false;
}

bool CodeGenerator::isUnsignedValue(llvm::Value* v) const {
    llvm::Value* base = v;
    // Trace through loads and zero-extending casts to find the alloca name.
    while (base) {
        if (auto* li = llvm::dyn_cast<llvm::LoadInst>(base)) {
            base = li->getPointerOperand();
            continue;
        }
        if (auto* cast = llvm::dyn_cast<llvm::CastInst>(base)) {
            if (cast->getOpcode() == llvm::Instruction::ZExt ||
                cast->getOpcode() == llvm::Instruction::BitCast) {
                base = cast->getOperand(0);
                continue;
            }
        }
        break;
    }
    if (!base) return false;
    const llvm::StringRef name = base->getName();
    if (name.empty()) return false;
    auto it = varTypeAnnotations_.find(name);
    if (it != varTypeAnnotations_.end())
        return isUnsignedAnnot(it->second);
    return false;
}

void CodeGenerator::checkConstModification(const std::string& name, const std::string& action) {
    auto constIt = constValues.find(name);
    if (constIt != constValues.end() && constIt->second) {
        // Give a more specific error message for frozen variables
        if (frozenVars_.count(name)) {
            throw DiagnosticError(
                Diagnostic{DiagnosticSeverity::Error, {"", 0, 0},
                           "Cannot " + action + " frozen variable '" + name +
                           "' — variable was frozen and is immutable for the rest of its lifetime"});
        }
        throw DiagnosticError(
            Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, "Cannot " + action + " const variable: " + name});
    }
    // Block writes to the source variable of an active mutable borrow.
    auto* bs = getBorrowStateOpt(name);
    if (bs && bs->mutBorrowed) {
        throw DiagnosticError(
            Diagnostic{DiagnosticSeverity::Error, {"", 0, 0},
                       "Cannot " + action + " variable '" + name +
                       "' — it has an active mutable borrow (release the borrow first)"});
    }
    // Block writes to source while any immutable borrows are alive.
    if (bs && bs->immutBorrowCount > 0) {
        throw DiagnosticError(
            Diagnostic{DiagnosticSeverity::Error, {"", 0, 0},
                       "Cannot " + action + " variable '" + name +
                       "' — it has " + std::to_string(bs->immutBorrowCount) +
                       " active immutable borrow(s)"});
    }
}

void CodeGenerator::validateScopeStacksMatch(const char* location) {
    if (scopeStack.size() != constScopeStack.size()) {
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error,
                                         {"", 0, 0},
                                         "Scope tracking mismatch in codegen (" + std::string(location) +
                                             "): values=" + std::to_string(scopeStack.size()) +
                                             ", consts=" + std::to_string(constScopeStack.size())});
    }
}

llvm::AllocaInst* CodeGenerator::createEntryBlockAlloca(llvm::Function* function, const std::string& name,
                                                        llvm::Type* type) {
    llvm::IRBuilder<> entryBuilder(&function->getEntryBlock(), function->getEntryBlock().begin());
    auto* alloca = entryBuilder.CreateAlloca(type ? type : getDefaultType(), nullptr, name);
    // Set explicit alignment for i64 allocas — the default type in OmScript.
    llvm::Type* allocaType = type ? type : getDefaultType();
    if (allocaType->isIntegerTy(64) || allocaType->isDoubleTy() || allocaType->isPointerTy()) {
        alloca->setAlignment(llvm::Align(8));
    }
    return alloca;
}

void CodeGenerator::codegenError(const std::string& message, const ASTNode* node) {
    SourceLocation loc;
    if (node && node->line > 0) {
        loc.line = node->line;
        loc.column = node->column;
    }
    throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error, loc, message});
}

// ---------------------------------------------------------------------------

llvm::Function* CodeGenerator::declareExternalFn(llvm::StringRef name, llvm::FunctionType* ty) {
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    return fn;
}

void CodeGenerator::attachLoopMetadata(llvm::BranchInst* backEdgeBr) {
    if (optimizationLevel < OptimizationLevel::O1)
        return;
    llvm::SmallVector<llvm::Metadata*, 2> mds;
    mds.push_back(nullptr);
    mds.push_back(llvm::MDNode::get(*context,
        {llvm::MDString::get(*context, "llvm.loop.mustprogress")}));
    llvm::MDNode* md = llvm::MDNode::get(*context, mds);
    md->replaceOperandWith(0, md);
    backEdgeBr->setMetadata(llvm::LLVMContext::MD_loop, md);
}

void CodeGenerator::attachLoopMetadataVec(llvm::BranchInst* backEdgeBr,
                                           unsigned interleaveCount) {
    if (optimizationLevel < OptimizationLevel::O1)
        return;
    llvm::SmallVector<llvm::Metadata*, 4> mds;
    mds.push_back(nullptr);
    mds.push_back(llvm::MDNode::get(*context,
        {llvm::MDString::get(*context, "llvm.loop.mustprogress")}));
    if (optimizationLevel >= OptimizationLevel::O2) {
        mds.push_back(llvm::MDNode::get(*context,
            {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
             llvm::ConstantAsMetadata::get(
                 llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
        if (interleaveCount > 0) {
            mds.push_back(llvm::MDNode::get(*context,
                {llvm::MDString::get(*context, "llvm.loop.interleave.count"),
                 llvm::ConstantAsMetadata::get(
                     llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context),
                                            interleaveCount))}));
        }
    }
    llvm::MDNode* md = llvm::MDNode::get(*context, mds);
    md->replaceOperandWith(0, md);
    backEdgeBr->setMetadata(llvm::LLVMContext::MD_loop, md);
}

CodeGenerator::CountingLoopInfo CodeGenerator::emitCountingLoop(
        llvm::StringRef prefix,
        llvm::Value* limit,
        llvm::Value* start,
        unsigned interleaveCount,
        const std::function<void(llvm::PHINode*, llvm::BasicBlock*)>& bodyFn) {
    llvm::Function* function  = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* preheader = builder->GetInsertBlock();

    llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context,
        llvm::Twine(prefix) + ".loop", function);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context,
        llvm::Twine(prefix) + ".body", function);
    llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context,
        llvm::Twine(prefix) + ".done", function);

    // Jump from preheader into loop header.
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

    // Loop header: PHI + condition check.
    builder->SetInsertPoint(loopBB);
    llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2,
                                             llvm::Twine(prefix) + ".idx");
    idx->addIncoming(start, preheader);

    llvm::Value* cond = builder->CreateICmpULT(idx, limit,
                                                llvm::Twine(prefix) + ".cond");
    // Loops almost never execute zero iterations; mark the taken branch hot.
    if (optimizationLevel >= OptimizationLevel::O2) {
        llvm::MDNode* w = llvm::MDBuilder(*context).createBranchWeights(2000, 1);
        builder->CreateCondBr(cond, bodyBB, doneBB, w);
    } else {
        builder->CreateCondBr(cond, bodyBB, doneBB);
    }

    // Body: delegate to caller.  After bodyFn returns the insert point must
    builder->SetInsertPoint(bodyBB);
    bodyFn(idx, loopBB);

    // If the body didn't already jump somewhere (e.g. it always ends with a
    // CreateBr(loopBB)), leave the insert point in doneBB for the caller.
    builder->SetInsertPoint(doneBB);
    (void)interleaveCount; // interleaveCount is passed to bodyFn via capture
    return {idx, doneBB};
}

// ── IR emit helpers ───────────────────────────────────────────────────────────

llvm::Value* CodeGenerator::emitLoadArrayLen(llvm::Value* arrPtr,
                                              const llvm::Twine& name) {
    auto* load = builder->CreateAlignedLoad(getDefaultType(), arrPtr,
                                             llvm::MaybeAlign(8), name);
    load->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
    load->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
    nonNegValues_.insert(load);
    return load;
}

llvm::LoadInst* CodeGenerator::emitLoadArrayElem(llvm::Value* elemPtr,
                                               const llvm::Twine& name) {
    auto* load = builder->CreateAlignedLoad(getDefaultType(), elemPtr,
                                             llvm::MaybeAlign(8), name);
    load->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
    return load;
}

void CodeGenerator::emitStoreArrayLen(llvm::Value* len, llvm::Value* arrPtr) {
    auto* st = builder->CreateStore(len, arrPtr);
    st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
}

llvm::StoreInst* CodeGenerator::emitStoreArrayElem(llvm::Value* val, llvm::Value* elemPtr) {
    auto* st = builder->CreateAlignedStore(val, elemPtr, llvm::MaybeAlign(8));
    st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
    return st;
}

llvm::Value* CodeGenerator::emitAllocArray(llvm::Value* len,
                                            const llvm::Twine& name) {
    llvm::Value* one   = llvm::ConstantInt::get(getDefaultType(), 1);
    llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
    llvm::Value* slots = builder->CreateAdd(len, one,   name + ".slots",
                                             /*NUW=*/true, /*NSW=*/true);
    llvm::Value* bytes = builder->CreateMul(slots, eight, name + ".bytes",
                                             /*NUW=*/true, /*NSW=*/true);
    llvm::Value* buf   = builder->CreateCall(getOrDeclareMalloc(), {bytes},
                                              name + ".buf");
    llvm::cast<llvm::CallInst>(buf)->addRetAttr(
        llvm::Attribute::getWithDereferenceableBytes(*context, 8));
    emitStoreArrayLen(len, buf);
    return buf;
}

llvm::Value* CodeGenerator::emitToArrayPtr(llvm::Value* val,
                                            const llvm::Twine& name) {
    val = toDefaultType(val);
    return builder->CreateIntToPtr(val, llvm::PointerType::getUnqual(*context),
                                   name);
}

llvm::Function* CodeGenerator::getOrDeclareStrlen() {
    if (auto* fn = module->getFunction("strlen"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(getDefaultType(), {ptrTy}, false);
    llvm::Function* fn = declareExternalFn("strlen", ty);
    // memory(argmem: read): strlen only reads through its pointer arg.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    OMSC_ADD_NOCAPTURE(fn, 0);                      // does not capture its pointer arg
    fn->addParamAttr(0, llvm::Attribute::ReadOnly); // only reads through the pointer
    fn->addParamAttr(0, llvm::Attribute::NonNull);  // never called with null
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareMalloc() {
    if (auto* fn = module->getFunction("malloc"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::PointerType::getUnqual(*context), {getDefaultType()}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "malloc", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoBuiltin);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull);
    // allocsize(0): parameter 0 is the allocation size — enables LLVM to
    fn->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(*context, 0, std::nullopt));
    // allockind("alloc,uninitialized"): malloc returns freshly allocated
    // memory; LLVM treats it as an allocation function for AA and LICM.
    fn->addFnAttr(llvm::Attribute::get(*context, llvm::Attribute::AllocKind,
                                       static_cast<uint64_t>(llvm::AllocFnKind::Alloc |
                                                             llvm::AllocFnKind::Uninitialized)));
    // inaccessiblememonly: malloc only touches allocator-internal state
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects::inaccessibleMemOnly()));
    // align(16): malloc on 64-bit Linux/macOS (glibc, musl, Darwin) guarantees
    fn->addRetAttr(llvm::Attribute::getWithAlignment(*context, llvm::Align(16)));
    return fn;
}

// aligned_alloc(size_t alignment, size_t size) -> void*
llvm::Function* CodeGenerator::getOrDeclareAlignedAlloc() {
    if (auto* fn = module->getFunction("aligned_alloc"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {getDefaultType(), getDefaultType()}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage,
                                                "aligned_alloc", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoBuiltin);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull);
    fn->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(*context, 1, std::nullopt));
    fn->addFnAttr(llvm::Attribute::get(*context, llvm::Attribute::AllocKind,
                                       static_cast<uint64_t>(llvm::AllocFnKind::Alloc |
                                                             llvm::AllocFnKind::Uninitialized)));
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects::inaccessibleMemOnly()));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareCalloc() {
    if (auto* fn = module->getFunction("calloc"))
        return fn;
    // calloc(size_t count, size_t size) -> void*
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {getDefaultType(), getDefaultType()}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "calloc", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoBuiltin);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull); // OmScript assumes alloc always succeeds
    // allocsize(0, 1): allocation size is arg0 * arg1 — enables LLVM to
    fn->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(*context, 0, 1));
    // allockind("alloc,zeroed"): calloc returns zeroed freshly allocated memory.
    fn->addFnAttr(llvm::Attribute::get(*context, llvm::Attribute::AllocKind,
                                       static_cast<uint64_t>(llvm::AllocFnKind::Alloc |
                                                             llvm::AllocFnKind::Zeroed)));
    // inaccessiblememonly: allows LICM to hoist calloc calls out of loops
    // when the size arguments are loop-invariant.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects::inaccessibleMemOnly()));
    // align(16): calloc has the same alignment guarantee as malloc on 64-bit POSIX.
    fn->addRetAttr(llvm::Attribute::getWithAlignment(*context, llvm::Align(16)));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrcpy() {
    if (auto* fn = module->getFunction("strcpy"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
    llvm::Function* fn = declareExternalFn("strcpy", ty);
    // memory(argmem: readwrite): strcpy only accesses memory through its pointer args.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly()));
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::Returned); // strcpy returns the destination pointer
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrcat() {
    if (auto* fn = module->getFunction("strcat"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
    llvm::Function* fn = declareExternalFn("strcat", ty);
    // memory(argmem: readwrite): strcat only accesses memory through its pointer args.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly()));
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::Returned); // strcat returns the destination pointer
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrcmp() {
    if (auto* fn = module->getFunction("strcmp"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(builder->getInt32Ty(), {ptrTy, ptrTy}, false);
    llvm::Function* fn = declareExternalFn("strcmp", ty);
    // memory(argmem: read): strcmp only reads through its pointer args.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrncmp() {
    if (auto* fn = module->getFunction("strncmp"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* sizeTy = getDefaultType();  // size_t mapped to i64
    auto* ty = llvm::FunctionType::get(builder->getInt32Ty(), {ptrTy, ptrTy, sizeTy}, false);
    llvm::Function* fn = declareExternalFn("strncmp", ty);
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareMemcmp() {
    if (auto* fn = module->getFunction("memcmp"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* sizeTy = getDefaultType();  // size_t mapped to i64
    auto* ty = llvm::FunctionType::get(builder->getInt32Ty(), {ptrTy, ptrTy, sizeTy}, false);
    llvm::Function* fn = declareExternalFn("memcmp", ty);
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclarePutchar() {
    if (auto* fn = module->getFunction("putchar"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "putchar", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclarePuts() {
    if (auto* fn = module->getFunction("puts"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context),
                                       {llvm::PointerType::getUnqual(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "puts", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFputs() {
    if (auto* fn = module->getFunction("fputs"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    // fputs(const char* str, FILE* stream) -> int
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context),
                                       {ptrTy, ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fputs", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    OMSC_ADD_NOCAPTURE(fn, 1);  // stream pointer is not captured
    return fn;
}

llvm::Value* CodeGenerator::getOrDeclareStdout() {
    // Get the C library 'stdout' global (extern FILE *stdout).
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    if (auto* gv = module->getGlobalVariable("stdout")) {
        auto* load = builder->CreateLoad(ptrTy, gv, "stdout.val");
        // stdout is a process-wide constant — mark as nonnull and invariant
        // so LLVM can hoist/CSE repeated loads.
        load->setMetadata(llvm::LLVMContext::MD_nonnull,
                          llvm::MDNode::get(*context, {}));
        return load;
    }
    auto* gv = new llvm::GlobalVariable(
        *module, ptrTy, /*isConstant=*/false,
        llvm::GlobalValue::ExternalLinkage, nullptr, "stdout");
    auto* load = builder->CreateLoad(ptrTy, gv, "stdout.val");
    load->setMetadata(llvm::LLVMContext::MD_nonnull,
                      llvm::MDNode::get(*context, {}));
    return load;
}

llvm::Function* CodeGenerator::getOrDeclareScanf() {
    if (auto* fn = module->getFunction("scanf"))
        return fn;
    auto* ty =
        llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {llvm::PointerType::getUnqual(*context)}, true);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "scanf", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareExit() {
    if (auto* fn = module->getFunction("exit"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context), {llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "exit", module.get());
    fn->addFnAttr(llvm::Attribute::NoReturn);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    // cold: exit() is an error-handling/termination path — marking it cold
    fn->addFnAttr(llvm::Attribute::Cold);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareAbort() {
    if (auto* fn = module->getFunction("abort"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context), false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "abort", module.get());
    fn->addFnAttr(llvm::Attribute::NoReturn);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    // cold: abort() is an error-handling path — marking it cold improves
    // code layout by keeping the abort call site out of hot I-cache lines.
    fn->addFnAttr(llvm::Attribute::Cold);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareSnprintf() {
    if (auto* fn = module->getFunction("snprintf"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy, getDefaultType(), ptrTy}, true);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "snprintf", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    // snprintf: dest is writeonly+nocapture, format string is readonly+nocapture.
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(2, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 2);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareMemchr() {
    if (auto* fn = module->getFunction("memchr"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, llvm::Type::getInt32Ty(*context), getDefaultType()}, false);
    llvm::Function* fn = declareExternalFn("memchr", ty);
    // memory(argmem: read): memchr only reads through its pointer arg.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFree() {
    if (auto* fn = module->getFunction("free"))
        return fn;
    auto* ty =
        llvm::FunctionType::get(llvm::Type::getVoidTy(*context), {llvm::PointerType::getUnqual(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "free", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    OMSC_ADD_NOCAPTURE(fn, 0); // free does not capture its pointer argument
    // allockind("free"): marks free as a deallocation function — enables the
    // optimizer to pair malloc/free and eliminate dead allocation sequences.
    fn->addFnAttr(llvm::Attribute::get(*context, llvm::Attribute::AllocKind,
                                       static_cast<uint64_t>(llvm::AllocFnKind::Free)));
    // memory(argmem: readwrite, inaccessiblemem: readwrite): free reads the
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects::inaccessibleOrArgMemOnly()));
    fn->addParamAttr(0, llvm::Attribute::get(*context, llvm::Attribute::AllocatedPointer));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrstr() {
    if (auto* fn = module->getFunction("strstr"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
    llvm::Function* fn = declareExternalFn("strstr", ty);
    // memory(argmem: read): strstr only reads through its pointer args.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareMemcpy() {
    if (auto* fn = module->getFunction("memcpy"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy, getDefaultType()}, false);
    llvm::Function* fn = declareExternalFn("memcpy", ty);
    // memory(argmem: readwrite): memcpy only accesses memory through its pointer args.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly()));
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::Returned); // memcpy returns the destination pointer
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareMemmove() {
    if (auto* fn = module->getFunction("memmove"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy, getDefaultType()}, false);
    llvm::Function* fn = declareExternalFn("memmove", ty);
    // memory(argmem: readwrite): memmove only accesses memory through its pointer args.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly()));
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::Returned); // memmove returns the destination pointer
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareToupper() {
    if (auto* fn = module->getFunction("toupper"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = declareExternalFn("toupper", ty);
    // memory(none): toupper is a pure function — no reads or writes to memory.
    // This lets the optimizer hoist it out of loops and CSE duplicate calls.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::none()));
    // speculatable: toupper has no side effects, LLVM may hoist/CSE calls.
    fn->addFnAttr(llvm::Attribute::Speculatable);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareTolower() {
    if (auto* fn = module->getFunction("tolower"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = declareExternalFn("tolower", ty);
    // memory(none): tolower is a pure function — no reads or writes to memory.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::none()));
    fn->addFnAttr(llvm::Attribute::Speculatable);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareIsspace() {
    if (auto* fn = module->getFunction("isspace"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = declareExternalFn("isspace", ty);
    // memory(none): isspace is a pure function — no reads or writes to memory.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::none()));
    fn->addFnAttr(llvm::Attribute::Speculatable);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrtoll() {
    if (auto* fn = module->getFunction("strtoll"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(getDefaultType(), {ptrTy, ptrTy, llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = declareExternalFn("strtoll", ty);
    // memory(argmem: read): strtoll reads the string via param 0; param 1
    // (endptr) is always null at OmScript call sites so no write occurs.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    // param 1 (endptr) is always passed as null in OmScript — nocapture is
    // still valid (the pointer itself is never stored anywhere by strtoll).
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrtod() {
    if (auto* fn = module->getFunction("strtod"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(getFloatType(), {ptrTy, ptrTy}, false);
    llvm::Function* fn = declareExternalFn("strtod", ty);
    // memory(argmem: read): strtod reads the string via param 0; param 1
    // (endptr) is always null at OmScript call sites so no write occurs.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrdup() {
    if (auto* fn = module->getFunction("strdup"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    llvm::Function* fn = declareExternalFn("strdup", ty);
    // memory(argmem: read, inaccessiblemem: readwrite): strdup reads the source
    // string and allocates new memory through the heap allocator.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects(
            llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref) |
            llvm::MemoryEffects::inaccessibleMemOnly())));
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFloor() {
    if (auto* fn = module->getFunction("floor"))
        return fn;
    auto* ty = llvm::FunctionType::get(getFloatType(), {getFloatType()}, false);
    llvm::Function* fn = declareExternalFn("floor", ty);
    // memory(none): floor is a pure mathematical function.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::none()));
    // speculatable: floor never reads/writes memory or has side effects, so
    // LLVM may hoist or speculate calls across branches and into loop preheaders.
    fn->addFnAttr(llvm::Attribute::Speculatable);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareCeil() {
    if (auto* fn = module->getFunction("ceil"))
        return fn;
    auto* ty = llvm::FunctionType::get(getFloatType(), {getFloatType()}, false);
    llvm::Function* fn = declareExternalFn("ceil", ty);
    // memory(none): ceil is a pure mathematical function.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::none()));
    // speculatable: same rationale as floor.
    fn->addFnAttr(llvm::Attribute::Speculatable);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareRound() {
    if (auto* fn = module->getFunction("round"))
        return fn;
    auto* ty = llvm::FunctionType::get(getFloatType(), {getFloatType()}, false);
    llvm::Function* fn = declareExternalFn("round", ty);
    // memory(none): round is a pure mathematical function.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::none()));
    // speculatable: same rationale as floor.
    fn->addFnAttr(llvm::Attribute::Speculatable);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareQsort() {
    if (auto* fn = module->getFunction("qsort"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
                                       {ptrTy, getDefaultType(), getDefaultType(), ptrTy}, false);
    llvm::Function* fn = declareExternalFn("qsort", ty);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareRand() {
    if (auto* fn = module->getFunction("rand"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "rand", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    // rand() only accesses global PRNG state (inaccessible to the caller);
    // this lets LLVM reorder it past pure memory operations.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context,
        llvm::MemoryEffects::inaccessibleMemOnly()));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareSrand() {
    if (auto* fn = module->getFunction("srand"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context), {llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "srand", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    // Deliberately *not* adding NoSync: srand mutates a hidden global PRNG
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context,
        llvm::MemoryEffects::inaccessibleMemOnly()));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareTimeFunc() {
    if (auto* fn = module->getFunction("time"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(getDefaultType(), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "time", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    // memory(inaccessiblemem: read): time() reads the OS clock (inaccessible
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context,
        llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Ref)));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareUsleep() {
    if (auto* fn = module->getFunction("usleep"))
        return fn;
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "usleep", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrchr() {
    if (auto* fn = module->getFunction("strchr"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = declareExternalFn("strchr", ty);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    // memory(argmem: read): strchr only reads through its pointer arg.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareStrndup() {
    if (auto* fn = module->getFunction("strndup"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, getDefaultType()}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "strndup", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull);  // strndup never returns null (we abort on failure)
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(0, llvm::Attribute::NonNull);  // source string is never null
    // allocsize(1): parameter 1 upper-bounds the allocation size — enables LLVM
    // to reason about the returned buffer size for alias analysis.
    fn->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(*context, 1, std::nullopt));
    // allockind("alloc,uninitialized"): strndup allocates new heap memory (the
    fn->addFnAttr(llvm::Attribute::get(*context, llvm::Attribute::AllocKind,
                                       static_cast<uint64_t>(llvm::AllocFnKind::Alloc |
                                                             llvm::AllocFnKind::Uninitialized)));
    // memory(argmem: read, inaccessiblemem: readwrite): strndup reads the source
    // string and writes to allocator-internal structures (same as strdup).
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects(
            llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref) |
            llvm::MemoryEffects::inaccessibleMemOnly())));
    // align(16): strndup allocates via malloc internally, inheriting the same
    // 16-byte alignment guarantee on 64-bit POSIX systems.
    fn->addRetAttr(llvm::Attribute::getWithAlignment(*context, llvm::Align(16)));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareRealloc() {
    if (auto* fn = module->getFunction("realloc"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, getDefaultType()}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "realloc", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull); // OmScript assumes alloc always succeeds
    OMSC_ADD_NOCAPTURE(fn, 0); // realloc does not capture the old pointer
    // allocsize(1): parameter 1 is the new allocation size.
    fn->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(*context, 1, std::nullopt));
    // allockind("realloc"): marks realloc as a reallocation function so LLVM can
    // track the pointer through resize operations for alias analysis.
    fn->addFnAttr(llvm::Attribute::get(*context, llvm::Attribute::AllocKind,
                                       static_cast<uint64_t>(llvm::AllocFnKind::Realloc)));
    // memory(argmem: readwrite, inaccessiblemem: readwrite): realloc reads the
    // old allocation and writes to allocator-internal structures.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        *context, llvm::MemoryEffects::inaccessibleOrArgMemOnly()));
    fn->addParamAttr(0, llvm::Attribute::get(*context, llvm::Attribute::AllocatedPointer));
    // align(16): realloc returns memory with the same alignment guarantee as malloc.
    fn->addRetAttr(llvm::Attribute::getWithAlignment(*context, llvm::Align(16)));
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareAtoi() {
    if (auto* fn = module->getFunction("atoi"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "atoi", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    // memory(argmem: read): atoi only reads through its pointer arg.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareAtof() {
    if (auto* fn = module->getFunction("atof"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getDoubleTy(*context), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "atof", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    // memory(argmem: read): atof only reads through its pointer arg.
    fn->addFnAttr(llvm::Attribute::getWithMemoryEffects(*context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFwrite() {
    if (auto* fn = module->getFunction("fwrite"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(getDefaultType(), {ptrTy, getDefaultType(), getDefaultType(), ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fwrite", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(3, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 3);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFflush() {
    if (auto* fn = module->getFunction("fflush"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fflush", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFgets() {
    if (auto* fn = module->getFunction("fgets"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, llvm::Type::getInt32Ty(*context), ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fgets", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(2, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 2);
    return fn;
}


llvm::Function* CodeGenerator::getOrDeclareFopen() {
    if (auto* fn = module->getFunction("fopen"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fopen", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NoAlias);  // fopen returns a new FILE* (or null)
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    fn->addParamAttr(1, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFclose() {
    if (auto* fn = module->getFunction("fclose"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fclose", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFread() {
    if (auto* fn = module->getFunction("fread"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(getDefaultType(), {ptrTy, getDefaultType(), getDefaultType(), ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fread", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(3, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 3);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFseek() {
    if (auto* fn = module->getFunction("fseek"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context),
                                       {ptrTy, getDefaultType(), llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fseek", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareFtell() {
    if (auto* fn = module->getFunction("ftell"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(getDefaultType(), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "ftell", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareAccess() {
    if (auto* fn = module->getFunction("access"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty =
        llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy, llvm::Type::getInt32Ty(*context)}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "access", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

// ---------------------------------------------------------------------------

llvm::Function* CodeGenerator::getOrDeclarePthreadCreate() {
    if (auto* fn = module->getFunction("pthread_create"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    // int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
    //                    void *(*start_routine)(void*), void *arg)
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy, ptrTy, ptrTy, ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_create", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::NoFree);
    OMSC_ADD_NOCAPTURE(fn, 0);  // thread ID output pointer
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 1);  // attributes (usually null)
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclarePthreadJoin() {
    if (auto* fn = module->getFunction("pthread_join"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    // int pthread_join(pthread_t thread, void **retval)
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {getDefaultType(), ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_join", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::NoFree);
    OMSC_ADD_NOCAPTURE(fn, 1);  // retval output pointer
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclarePthreadMutexInit() {
    if (auto* fn = module->getFunction("pthread_mutex_init"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    // int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy, ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_mutex_init", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::NoFree);
    OMSC_ADD_NOCAPTURE(fn, 0);
    OMSC_ADD_NOCAPTURE(fn, 1);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclarePthreadMutexLock() {
    if (auto* fn = module->getFunction("pthread_mutex_lock"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_mutex_lock", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::NoFree);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclarePthreadMutexUnlock() {
    if (auto* fn = module->getFunction("pthread_mutex_unlock"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_mutex_unlock", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::NoFree);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclarePthreadMutexDestroy() {
    if (auto* fn = module->getFunction("pthread_mutex_destroy"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "pthread_mutex_destroy", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::NoFree);
    OMSC_ADD_NOCAPTURE(fn, 0);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareGetenv() {
    if (auto* fn = module->getFunction("getenv"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "getenv", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    fn->addFnAttr(llvm::Attribute::NoFree);
    fn->addFnAttr(llvm::Attribute::NoSync);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    OMSC_ADD_NOCAPTURE(fn, 0);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareSetenv() {
    if (auto* fn = module->getFunction("setenv"))
        return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i32Ty = llvm::Type::getInt32Ty(*context);
    auto* ty = llvm::FunctionType::get(i32Ty, {ptrTy, ptrTy, i32Ty}, false);
    llvm::Function* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "setenv", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(1, llvm::Attribute::NonNull);
    OMSC_ADD_NOCAPTURE(fn, 0);
    OMSC_ADD_NOCAPTURE(fn, 1);
    return fn;
}
// ── BigInt runtime helper declarations ──────────────────────────────────────

namespace {
/// Shorthand: (ptr) -> ptr — the most common bigint binary-op signature.
static llvm::FunctionType* bigintBinaryTy(llvm::LLVMContext& ctx) {
    auto* p = llvm::PointerType::getUnqual(ctx);
    return llvm::FunctionType::get(p, {p, p}, false);
}
/// Shorthand: (ptr) -> ptr — the unary signature.
static llvm::FunctionType* bigintUnaryTy(llvm::LLVMContext& ctx) {
    auto* p = llvm::PointerType::getUnqual(ctx);
    return llvm::FunctionType::get(p, {p}, false);
}
/// Shorthand: (ptr, ptr) -> i32 — comparison returning int.
static llvm::FunctionType* bigintCmpTy(llvm::LLVMContext& ctx) {
    auto* p = llvm::PointerType::getUnqual(ctx);
    return llvm::FunctionType::get(llvm::Type::getInt32Ty(ctx), {p, p}, false);
}
} // namespace

llvm::Function* CodeGenerator::getOrDeclareBigintNewI64() {
    if (auto* fn = module->getFunction("omsc_bigint_new_i64")) return fn;
    auto* i64Ty = llvm::Type::getInt64Ty(*context);
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
    auto* fn = declareExternalFn("omsc_bigint_new_i64", ty);
    fn->removeFnAttr(llvm::Attribute::NoFree); // allocates
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareBigintNewStr() {
    if (auto* fn = module->getFunction("omsc_bigint_new_str")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    auto* fn = declareExternalFn("omsc_bigint_new_str", ty);
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareBigintFree() {
    if (auto* fn = module->getFunction("omsc_bigint_free")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context), {ptrTy}, false);
    auto* fn = llvm::Function::Create(ty, llvm::Function::ExternalLinkage,
                                      "omsc_bigint_free", module.get());
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    return fn;
}

llvm::Function* CodeGenerator::getOrDeclareBigintAdd() {
    if (auto* fn = module->getFunction("omsc_bigint_add")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_add", bigintBinaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintSub() {
    if (auto* fn = module->getFunction("omsc_bigint_sub")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_sub", bigintBinaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintMul() {
    if (auto* fn = module->getFunction("omsc_bigint_mul")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_mul", bigintBinaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintDiv() {
    if (auto* fn = module->getFunction("omsc_bigint_div")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_div", bigintBinaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintMod() {
    if (auto* fn = module->getFunction("omsc_bigint_mod")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_mod", bigintBinaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintNeg() {
    if (auto* fn = module->getFunction("omsc_bigint_neg")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_neg", bigintUnaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintAbs() {
    if (auto* fn = module->getFunction("omsc_bigint_abs")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_abs", bigintUnaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintPow() {
    if (auto* fn = module->getFunction("omsc_bigint_pow")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_pow", bigintBinaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintGcd() {
    if (auto* fn = module->getFunction("omsc_bigint_gcd")) return fn;
    auto* fn = declareExternalFn("omsc_bigint_gcd", bigintBinaryTy(*context));
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintEq() {
    if (auto* fn = module->getFunction("omsc_bigint_eq")) return fn;
    return declareExternalFn("omsc_bigint_eq", bigintCmpTy(*context));
}
llvm::Function* CodeGenerator::getOrDeclareBigintLt() {
    if (auto* fn = module->getFunction("omsc_bigint_lt")) return fn;
    return declareExternalFn("omsc_bigint_lt", bigintCmpTy(*context));
}
llvm::Function* CodeGenerator::getOrDeclareBigintLe() {
    if (auto* fn = module->getFunction("omsc_bigint_le")) return fn;
    return declareExternalFn("omsc_bigint_le", bigintCmpTy(*context));
}
llvm::Function* CodeGenerator::getOrDeclareBigintGt() {
    if (auto* fn = module->getFunction("omsc_bigint_gt")) return fn;
    return declareExternalFn("omsc_bigint_gt", bigintCmpTy(*context));
}
llvm::Function* CodeGenerator::getOrDeclareBigintGe() {
    if (auto* fn = module->getFunction("omsc_bigint_ge")) return fn;
    return declareExternalFn("omsc_bigint_ge", bigintCmpTy(*context));
}
llvm::Function* CodeGenerator::getOrDeclareBigintCmp() {
    if (auto* fn = module->getFunction("omsc_bigint_cmp")) return fn;
    return declareExternalFn("omsc_bigint_cmp", bigintCmpTy(*context));
}
llvm::Function* CodeGenerator::getOrDeclareBigintTostring() {
    if (auto* fn = module->getFunction("omsc_bigint_tostring")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    auto* fn = declareExternalFn("omsc_bigint_tostring", ty);
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintToI64() {
    if (auto* fn = module->getFunction("omsc_bigint_to_i64")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt64Ty(*context), {ptrTy}, false);
    return declareExternalFn("omsc_bigint_to_i64", ty);
}
llvm::Function* CodeGenerator::getOrDeclareBigintBitLength() {
    if (auto* fn = module->getFunction("omsc_bigint_bit_length")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* ty = llvm::FunctionType::get(llvm::Type::getInt64Ty(*context), {ptrTy}, false);
    return declareExternalFn("omsc_bigint_bit_length", ty);
}
llvm::Function* CodeGenerator::getOrDeclareBigintIsZero() {
    if (auto* fn = module->getFunction("omsc_bigint_is_zero")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i32Ty = llvm::Type::getInt32Ty(*context);
    auto* ty = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
    return declareExternalFn("omsc_bigint_is_zero", ty);
}
llvm::Function* CodeGenerator::getOrDeclareBigintIsNegative() {
    if (auto* fn = module->getFunction("omsc_bigint_is_negative")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i32Ty = llvm::Type::getInt32Ty(*context);
    auto* ty = llvm::FunctionType::get(i32Ty, {ptrTy}, false);
    return declareExternalFn("omsc_bigint_is_negative", ty);
}
llvm::Function* CodeGenerator::getOrDeclareBigintShl() {
    if (auto* fn = module->getFunction("omsc_bigint_shl")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = llvm::Type::getInt64Ty(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty}, false);
    auto* fn = declareExternalFn("omsc_bigint_shl", ty);
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
llvm::Function* CodeGenerator::getOrDeclareBigintShr() {
    if (auto* fn = module->getFunction("omsc_bigint_shr")) return fn;
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = llvm::Type::getInt64Ty(*context);
    auto* ty = llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty}, false);
    auto* fn = declareExternalFn("omsc_bigint_shr", ty);
    fn->removeFnAttr(llvm::Attribute::NoFree);
    fn->addRetAttr(llvm::Attribute::NonNull);
    return fn;
}
// ---------------------------------------------------------------------------

/// Emit the Rotate-Accumulate hash for a 64-bit integer key.
llvm::Value* CodeGenerator::emitKeyHash(llvm::Value* key) {
    auto* i64Ty = getDefaultType();

    // Step 1: multiply by large odd constant — primary avalanche.
    llvm::Value* h = builder->CreateMul(
        key, llvm::ConstantInt::get(i64Ty, 0xD6E8FEB86659FD93ULL), "h.mul");

    // Step 2: rotate right by 37 (prime, coprime to 64) — lossless permutation.
    llvm::Function* fshr = OMSC_GET_INTRINSIC(
        module.get(), llvm::Intrinsic::fshr, {i64Ty});
    llvm::Value* rotated = builder->CreateCall(
        fshr, {h, h, llvm::ConstantInt::get(i64Ty, 37)}, "h.rot");

    // Step 3: add (not xor) — carry propagation provides additional mixing.
    h = builder->CreateAdd(h, rotated, "h.mix", /*HasNUW=*/true, /*HasNSW=*/false);

    // Step 4: ensure hash >= 2 (0=empty, 1=tombstone are reserved).
    return builder->CreateOr(h, llvm::ConstantInt::get(i64Ty, 2), "hash");
}

/// Helper: set common attributes on emitted map functions.
static void setHashMapFnAttrs(llvm::Function* fn) {
    fn->setLinkage(llvm::Function::InternalLinkage);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::WillReturn);
    // AlwaysInline: these are small helpers (typically < 50 LLVM IR instructions)
    fn->addFnAttr(llvm::Attribute::AlwaysInline);
}

// __omsc_hmap_new() -> ptr
//   Allocates a hash table with capacity 8, size 0, all entries zeroed.
llvm::Function* CodeGenerator::getOrEmitHashMapNew() {
    if (auto* fn = module->getFunction("__omsc_hmap_new"))
        return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = getDefaultType();
    auto* fnTy = llvm::FunctionType::get(ptrTy, {}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__omsc_hmap_new", module.get());
    setHashMapFnAttrs(fn);
    // Returns a fresh calloc'd allocation — always unique and non-null.
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull);

    auto savedIP = builder->saveIP();
    auto* entryBB = llvm::BasicBlock::Create(*context, "entry", fn);
    builder->SetInsertPoint(entryBB);

    // capacity = 8, total slots = 2 + 3*8 = 26, bytes = 26*8 = 208
    static constexpr int64_t kInitCapacity    = 8;
    static constexpr int64_t kSlotsPerBucket  = 3;    // key, value, hash
    static constexpr int64_t kHeaderSlots     = 2;    // length, capacity
    static constexpr int64_t kInitTotalSlots  = kHeaderSlots + kSlotsPerBucket * kInitCapacity;
    static constexpr int64_t kInitBytes       = kInitTotalSlots * 8LL;

    llvm::Value* cap = llvm::ConstantInt::get(i64Ty, kInitCapacity);
    llvm::Value* totalBytes = llvm::ConstantInt::get(i64Ty, kInitBytes);
    // Use calloc so all hash slots start as 0 (empty)
    llvm::Value* buf = builder->CreateCall(getOrDeclareCalloc(), {
        llvm::ConstantInt::get(i64Ty, 1), totalBytes
    }, "hmap.buf");
    // OmScript assumes allocations always succeed — annotate call-site result.
    llvm::cast<llvm::CallInst>(buf)->addRetAttr(llvm::Attribute::NonNull);
    llvm::cast<llvm::CallInst>(buf)->addRetAttr(
        llvm::Attribute::getWithDereferenceableBytes(*context, 208));
    // Store capacity in slot 0
    {   auto* st = builder->CreateAlignedStore(cap, buf, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_); }
    // Size (slot 1) is already 0 from calloc
    builder->CreateRet(buf);

    builder->restoreIP(savedIP);
    return fn;
}

// __omsc_hmap_set(map: ptr, key: i64, val: i64) -> ptr
//   Insert or update a key-value pair.  Returns the (possibly reallocated) map.
llvm::Function* CodeGenerator::getOrEmitHashMapSet() {
    if (auto* fn = module->getFunction("__omsc_hmap_set"))
        return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = getDefaultType();
    auto* fnTy = llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty, i64Ty}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__omsc_hmap_set", module.get());
    setHashMapFnAttrs(fn);
    // OmScript ownership: map pointer is the sole reference to this allocation.
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    // Returns the (possibly reallocated) map — still a unique allocation.
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull);
    fn->getArg(0)->setName("map");
    fn->getArg(1)->setName("key");
    fn->getArg(2)->setName("val");

    auto savedIP = builder->saveIP();

    // Basic blocks
    auto* entryBB    = llvm::BasicBlock::Create(*context, "entry", fn);
    auto* probeBB    = llvm::BasicBlock::Create(*context, "probe", fn);
    auto* checkBB    = llvm::BasicBlock::Create(*context, "check", fn);
    auto* emptyBB    = llvm::BasicBlock::Create(*context, "empty", fn);
    auto* occupBB    = llvm::BasicBlock::Create(*context, "occup", fn);
    auto* matchBB    = llvm::BasicBlock::Create(*context, "match", fn);
    auto* nextBB     = llvm::BasicBlock::Create(*context, "next", fn);
    auto* insertBB   = llvm::BasicBlock::Create(*context, "insert", fn);
    auto* growBB     = llvm::BasicBlock::Create(*context, "grow", fn);
    auto* doneBB     = llvm::BasicBlock::Create(*context, "done", fn);

    // Entry: compute hash, load capacity/size
    builder->SetInsertPoint(entryBB);
    llvm::Value* mapArg = fn->getArg(0);
    llvm::Value* keyArg = fn->getArg(1);
    llvm::Value* valArg = fn->getArg(2);

    // Rotate-Accumulate (RA) hash
    llvm::Value* hashVal = emitKeyHash(keyArg);
    auto* capPtr = mapArg;  // slot 0
    auto* capLoad = builder->CreateAlignedLoad(i64Ty, capPtr, llvm::MaybeAlign(8), "cap");
    capLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    llvm::Value* cap = capLoad;
    // Capacity is always a power of 2 >= 8.  Communicate this to LLVM:
    {
        llvm::Metadata* rangeMD[] = {
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 8)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 1ULL << 62))};
        capLoad->setMetadata(llvm::LLVMContext::MD_range,
                             llvm::MDNode::get(*context, rangeMD));
        llvm::Value* capM1 = builder->CreateSub(cap, llvm::ConstantInt::get(i64Ty, 1));
        llvm::Value* isPow2 = builder->CreateICmpEQ(
            builder->CreateAnd(cap, capM1), llvm::ConstantInt::get(i64Ty, 0), "cap.pow2");
        llvm::Function* assumeFn = OMSC_GET_INTRINSIC(
            module.get(), llvm::Intrinsic::assume, {});
        builder->CreateCall(assumeFn, {isPow2});
    }

    llvm::Value* sizePtr = builder->CreateInBoundsGEP(i64Ty, mapArg, llvm::ConstantInt::get(i64Ty, 1), "sizeptr");
    auto* sizeLoad = builder->CreateAlignedLoad(i64Ty, sizePtr, llvm::MaybeAlign(8), "size");
    sizeLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    {   // Size is always in [0, 2^62).
        llvm::Metadata* rangeMD[] = {
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 0)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 1ULL << 62))};
        sizeLoad->setMetadata(llvm::LLVMContext::MD_range,
                              llvm::MDNode::get(*context, rangeMD));
    }
    llvm::Value* size = sizeLoad;
    llvm::Value* mask = builder->CreateSub(cap, llvm::ConstantInt::get(i64Ty, 1), "mask", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* startSlot = builder->CreateAnd(hashVal, mask, "startslot");
    builder->CreateBr(probeBB);

    // Probe loop: linear probing
    builder->SetInsertPoint(probeBB);
    llvm::PHINode* slot = builder->CreatePHI(i64Ty, 2, "slot");
    slot->addIncoming(startSlot, entryBB);
    llvm::PHINode* firstTombstone = builder->CreatePHI(i64Ty, 2, "first.tomb");
    firstTombstone->addIncoming(llvm::ConstantInt::get(i64Ty, -1), entryBB);  // -1 = no tombstone seen

    // Compute entry base: mapPtr + 2 + slot*3
    llvm::Value* three = llvm::ConstantInt::get(i64Ty, 3);
    llvm::Value* entryOff = builder->CreateAdd(
        builder->CreateMul(slot, three, "slot3", /*HasNUW=*/true, /*HasNSW=*/true),
        llvm::ConstantInt::get(i64Ty, 2), "entryoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* hashPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, entryOff, "hashptr");
    llvm::Value* slotHash = builder->CreateAlignedLoad(i64Ty, hashPtr, llvm::MaybeAlign(8), "slothash");
    llvm::cast<llvm::Instruction>(slotHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    // Check if empty (hash == 0)
    llvm::Value* isEmpty = builder->CreateICmpEQ(slotHash, llvm::ConstantInt::get(i64Ty, 0), "isempty");
    builder->CreateCondBr(isEmpty, emptyBB, checkBB);

    // Check: is it occupied or tombstone?
    builder->SetInsertPoint(checkBB);
    llvm::Value* isTomb = builder->CreateICmpEQ(slotHash, llvm::ConstantInt::get(i64Ty, 1), "istomb");
    // Update first tombstone tracker
    llvm::Value* hasTomb = builder->CreateICmpNE(firstTombstone, llvm::ConstantInt::get(i64Ty, -1), "hastomb");
    llvm::Value* newTomb = builder->CreateSelect(
        builder->CreateAnd(isTomb, builder->CreateNot(hasTomb)),
        slot, firstTombstone, "newtomb");
    builder->CreateCondBr(isTomb, nextBB, occupBB);

    // Occupied: first compare stored hash vs computed hash (cheap i64 cmp).
    builder->SetInsertPoint(occupBB);
    auto* checkKeyBB = llvm::BasicBlock::Create(*context, "checkkey", fn);
    llvm::Value* hashMatch = builder->CreateICmpEQ(slotHash, hashVal, "hashmatch");
    // Hash match is unlikely on long probe chains; weight accordingly.
    auto* hashW = llvm::MDBuilder(*context).createBranchWeights(1, 4);
    builder->CreateCondBr(hashMatch, checkKeyBB, nextBB, hashW);

    // Hash matched — now load and compare the actual key.
    builder->SetInsertPoint(checkKeyBB);
    llvm::Value* keyOff = builder->CreateAdd(entryOff, llvm::ConstantInt::get(i64Ty, 1), "keyoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* eKeyPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, keyOff, "ekeyptr");
    llvm::Value* eKey = builder->CreateAlignedLoad(i64Ty, eKeyPtr, llvm::MaybeAlign(8), "ekey");
    llvm::cast<llvm::Instruction>(eKey)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
    llvm::Value* keyMatch = builder->CreateICmpEQ(eKey, keyArg, "keymatch");
    // When hashes match, key match is overwhelmingly likely (collision ~1/2^62).
    auto* keyW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
    builder->CreateCondBr(keyMatch, matchBB, nextBB, keyW);

    // Match: update value in place
    builder->SetInsertPoint(matchBB);
    llvm::Value* valOff = builder->CreateAdd(entryOff, llvm::ConstantInt::get(i64Ty, 2), "valoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* eValPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, valOff, "evalptr");
    {   auto* st = builder->CreateAlignedStore(valArg, eValPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_); }
    builder->CreateBr(doneBB);

    // Next: advance to next slot (linear probe), wrap around
    builder->SetInsertPoint(nextBB);
    llvm::PHINode* tombForNext = builder->CreatePHI(i64Ty, 3, "tomb.next");
    tombForNext->addIncoming(newTomb, checkBB);          // tombstone path: may update tracker
    tombForNext->addIncoming(firstTombstone, occupBB);   // hash mismatch: no change
    tombForNext->addIncoming(firstTombstone, checkKeyBB); // key mismatch: no change
    llvm::Value* nextSlot = builder->CreateAnd(
        builder->CreateAdd(slot, llvm::ConstantInt::get(i64Ty, 1), "slot1", /*HasNUW=*/true, /*HasNSW=*/true),
        mask, "nextslot");
    slot->addIncoming(nextSlot, nextBB);
    firstTombstone->addIncoming(tombForNext, nextBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(probeBB)));

    // Empty: key not found — insert
    builder->SetInsertPoint(emptyBB);
    // Check if we need to grow: size+1 > cap*3/4
    llvm::Value* newSize = builder->CreateAdd(size, llvm::ConstantInt::get(i64Ty, 1), "newsize", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* threshold = builder->CreateLShr(
        builder->CreateMul(cap, llvm::ConstantInt::get(i64Ty, 3), "cap3thr", /*HasNUW=*/true, /*HasNSW=*/true), llvm::ConstantInt::get(i64Ty, 2), "threshold");
    llvm::Value* needGrow = builder->CreateICmpUGT(newSize, threshold, "needgrow");
    // Growth is rare: only triggered when load factor exceeds 75%, which
    llvm::MDNode* growWeights = llvm::MDBuilder(*context).createBranchWeights(1, 1000);
    builder->CreateCondBr(needGrow, growBB, insertBB, growWeights);

    // Insert: write into the slot (prefer tombstone slot if available)
    builder->SetInsertPoint(insertBB);
    llvm::Value* hasTombForInsert = builder->CreateICmpNE(firstTombstone, llvm::ConstantInt::get(i64Ty, -1), "hastomb2");
    llvm::Value* insSlot = builder->CreateSelect(hasTombForInsert, firstTombstone, slot, "insslot");
    llvm::Value* insOff = builder->CreateAdd(
        builder->CreateMul(insSlot, three, "ins3", /*HasNUW=*/true, /*HasNSW=*/true),
        llvm::ConstantInt::get(i64Ty, 2), "insoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* insHashPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, insOff, "inshashptr");
    {   auto* st = builder->CreateAlignedStore(hashVal, insHashPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_); }
    llvm::Value* insKeyPtr = builder->CreateInBoundsGEP(i64Ty, mapArg,
        builder->CreateAdd(insOff, llvm::ConstantInt::get(i64Ty, 1), "inskeyoff", /*HasNUW=*/true, /*HasNSW=*/true), "inskeyptr");
    {   auto* st = builder->CreateAlignedStore(keyArg, insKeyPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_); }
    llvm::Value* insValPtr = builder->CreateInBoundsGEP(i64Ty, mapArg,
        builder->CreateAdd(insOff, llvm::ConstantInt::get(i64Ty, 2), "insvaloff", /*HasNUW=*/true, /*HasNSW=*/true), "insvalptr");
    {   auto* st = builder->CreateAlignedStore(valArg, insValPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_); }
    // Update size
    {   auto* st = builder->CreateAlignedStore(newSize, sizePtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_); }
    builder->CreateBr(doneBB);

    // Grow: double capacity, rehash all entries, then insert the new key
    builder->SetInsertPoint(growBB);
    llvm::Value* newCap = builder->CreateShl(cap, llvm::ConstantInt::get(i64Ty, 1), "newcap", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* newTotalSlots = builder->CreateAdd(
        builder->CreateMul(newCap, three, "cap3", /*HasNUW=*/true, /*HasNSW=*/true),
        llvm::ConstantInt::get(i64Ty, 2), "newtotalslots", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* newTotalBytes = builder->CreateMul(newTotalSlots, llvm::ConstantInt::get(i64Ty, 8), "newtotalbytes", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* newBuf = builder->CreateCall(getOrDeclareCalloc(), {
        llvm::ConstantInt::get(i64Ty, 1), newTotalBytes
    }, "newbuf");
    llvm::cast<llvm::CallInst>(newBuf)->addRetAttr(llvm::Attribute::NonNull);
    // Store new capacity and size+1
    {   auto* st = builder->CreateAlignedStore(newCap, newBuf, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_); }
    llvm::Value* newSizePtr = builder->CreateInBoundsGEP(i64Ty, newBuf, llvm::ConstantInt::get(i64Ty, 1));
    {   auto* st = builder->CreateAlignedStore(newSize, newSizePtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_); }
    // Rehash loop: iterate old entries, insert non-empty/non-tombstone into new table
    llvm::Value* newMask = builder->CreateSub(newCap, llvm::ConstantInt::get(i64Ty, 1), "newmask", /*HasNUW=*/true, /*HasNSW=*/true);
    auto* rehashLoopBB = llvm::BasicBlock::Create(*context, "rehash.loop", fn);
    auto* rehashBodyBB = llvm::BasicBlock::Create(*context, "rehash.body", fn);
    auto* rehashProbeBB = llvm::BasicBlock::Create(*context, "rehash.probe", fn);
    auto* rehashWriteBB = llvm::BasicBlock::Create(*context, "rehash.write", fn);
    auto* rehashNextBB = llvm::BasicBlock::Create(*context, "rehash.next", fn);
    auto* rehashDoneBB = llvm::BasicBlock::Create(*context, "rehash.done", fn);
    auto* insertNewBB  = llvm::BasicBlock::Create(*context, "insertnew", fn);

    builder->CreateBr(rehashLoopBB);
    builder->SetInsertPoint(rehashLoopBB);
    llvm::PHINode* ri = builder->CreatePHI(i64Ty, 2, "ri");
    ri->addIncoming(llvm::ConstantInt::get(i64Ty, 0), growBB);
    llvm::Value* riDone = builder->CreateICmpULT(ri, cap, "ridone");
    builder->CreateCondBr(riDone, rehashBodyBB, rehashDoneBB);

    // Rehash body: read old entry, skip empty/tombstone
    builder->SetInsertPoint(rehashBodyBB);
    llvm::Value* oldOff = builder->CreateAdd(
        builder->CreateMul(ri, three, "ri3", /*HasNUW=*/true, /*HasNSW=*/true),
        llvm::ConstantInt::get(i64Ty, 2), "oldoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* oldHashPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, oldOff, "oldhashptr");
    llvm::Value* oldHash = builder->CreateAlignedLoad(i64Ty, oldHashPtr, llvm::MaybeAlign(8), "oldhash");
    llvm::cast<llvm::Instruction>(oldHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    llvm::Value* isActive = builder->CreateICmpUGE(oldHash, llvm::ConstantInt::get(i64Ty, 2), "isactive");
    builder->CreateCondBr(isActive, rehashProbeBB, rehashNextBB);

    // Rehash probe: find empty slot in new table
    builder->SetInsertPoint(rehashProbeBB);
    llvm::Value* oldKeyPtr = builder->CreateInBoundsGEP(i64Ty, mapArg,
        builder->CreateAdd(oldOff, llvm::ConstantInt::get(i64Ty, 1), "oldkeyoff", /*HasNUW=*/true, /*HasNSW=*/true), "oldkeyptr");
    llvm::Value* oldKey = builder->CreateAlignedLoad(i64Ty, oldKeyPtr, llvm::MaybeAlign(8), "oldkey");
    llvm::cast<llvm::Instruction>(oldKey)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
    llvm::Value* oldValPtr = builder->CreateInBoundsGEP(i64Ty, mapArg,
        builder->CreateAdd(oldOff, llvm::ConstantInt::get(i64Ty, 2), "oldvaloff", /*HasNUW=*/true, /*HasNSW=*/true), "oldvalptr");
    llvm::Value* oldVal = builder->CreateAlignedLoad(i64Ty, oldValPtr, llvm::MaybeAlign(8), "oldval");
    llvm::cast<llvm::Instruction>(oldVal)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_);
    // Find empty slot in new table using oldHash
    llvm::Value* rStartSlot = builder->CreateAnd(oldHash, newMask, "rstartslot");

    // Inner probe loop for rehash: find empty slot in new table
    auto* rProbeLpBB  = llvm::BasicBlock::Create(*context, "rprobe.lp", fn);
    auto* rAdvanceBB  = llvm::BasicBlock::Create(*context, "rprobe.adv", fn);

    builder->CreateBr(rProbeLpBB);
    builder->SetInsertPoint(rProbeLpBB);
    llvm::PHINode* rs = builder->CreatePHI(i64Ty, 2, "rs");
    rs->addIncoming(rStartSlot, rehashProbeBB);
    llvm::Value* rEntryOff = builder->CreateAdd(
        builder->CreateMul(rs, three, "rs3", /*HasNUW=*/true, /*HasNSW=*/true),
        llvm::ConstantInt::get(i64Ty, 2), "rentryoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* rHashPtr = builder->CreateInBoundsGEP(i64Ty, newBuf, rEntryOff, "rhashptr");
    llvm::Value* rSlotHash = builder->CreateAlignedLoad(i64Ty, rHashPtr, llvm::MaybeAlign(8), "rslothash");
    llvm::cast<llvm::Instruction>(rSlotHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    llvm::Value* rIsEmpty = builder->CreateICmpEQ(rSlotHash, llvm::ConstantInt::get(i64Ty, 0), "risempty");
    builder->CreateCondBr(rIsEmpty, rehashWriteBB, rAdvanceBB);

    // Advance probe: compute next slot, loop back
    builder->SetInsertPoint(rAdvanceBB);
    llvm::Value* rNext = builder->CreateAnd(
        builder->CreateAdd(rs, llvm::ConstantInt::get(i64Ty, 1), "rs1", /*HasNUW=*/true, /*HasNSW=*/true),
        newMask, "rnext");
    rs->addIncoming(rNext, rAdvanceBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(rProbeLpBB)));

    // Write rehashed entry into new table
    builder->SetInsertPoint(rehashWriteBB);
    {   auto* st = builder->CreateAlignedStore(oldHash, rHashPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_); }
    llvm::Value* rKeyPtr = builder->CreateInBoundsGEP(i64Ty, newBuf,
        builder->CreateAdd(rEntryOff, llvm::ConstantInt::get(i64Ty, 1), "rkeyoff", /*HasNUW=*/true, /*HasNSW=*/true), "rkeyptr");
    {   auto* st = builder->CreateAlignedStore(oldKey, rKeyPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_); }
    llvm::Value* rValPtr = builder->CreateInBoundsGEP(i64Ty, newBuf,
        builder->CreateAdd(rEntryOff, llvm::ConstantInt::get(i64Ty, 2), "rvaloff", /*HasNUW=*/true, /*HasNSW=*/true), "rvalptr");
    {   auto* st = builder->CreateAlignedStore(oldVal, rValPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_); }
    builder->CreateBr(rehashNextBB);

    // Rehash next: advance outer loop
    builder->SetInsertPoint(rehashNextBB);
    llvm::Value* riNext = builder->CreateAdd(ri, llvm::ConstantInt::get(i64Ty, 1), "rinext", /*HasNUW=*/true, /*HasNSW=*/true);
    ri->addIncoming(riNext, rehashNextBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(rehashLoopBB)));

    // After rehash: insert the new key into the new table
    builder->SetInsertPoint(rehashDoneBB);
    // Free old buffer
    builder->CreateCall(getOrDeclareFree(), {mapArg});
    builder->CreateBr(insertNewBB);

    // Insert new key into grown table (same linear probe logic)
    builder->SetInsertPoint(insertNewBB);
    llvm::Value* nStartSlot = builder->CreateAnd(hashVal, newMask, "nstartslot");
    auto* nProbeLpBB = llvm::BasicBlock::Create(*context, "nprobe.lp", fn);
    builder->CreateBr(nProbeLpBB);
    builder->SetInsertPoint(nProbeLpBB);
    llvm::PHINode* ns = builder->CreatePHI(i64Ty, 2, "ns");
    ns->addIncoming(nStartSlot, insertNewBB);
    llvm::Value* nEntryOff = builder->CreateAdd(
        builder->CreateMul(ns, three, "ns3", /*HasNUW=*/true, /*HasNSW=*/true),
        llvm::ConstantInt::get(i64Ty, 2), "nentryoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* nHashPtr = builder->CreateInBoundsGEP(i64Ty, newBuf, nEntryOff, "nhashptr");
    llvm::Value* nSlotHash = builder->CreateAlignedLoad(i64Ty, nHashPtr, llvm::MaybeAlign(8), "nslothash");
    llvm::cast<llvm::Instruction>(nSlotHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    llvm::Value* nIsEmpty = builder->CreateICmpEQ(nSlotHash, llvm::ConstantInt::get(i64Ty, 0), "nisempty");
    auto* nWriteBB = llvm::BasicBlock::Create(*context, "nprobe.write", fn);
    auto* nAdvBB   = llvm::BasicBlock::Create(*context, "nprobe.adv", fn);
    builder->CreateCondBr(nIsEmpty, nWriteBB, nAdvBB);

    builder->SetInsertPoint(nAdvBB);
    llvm::Value* nNext = builder->CreateAnd(
        builder->CreateAdd(ns, llvm::ConstantInt::get(i64Ty, 1), "ns1", /*HasNUW=*/true, /*HasNSW=*/true),
        newMask, "nnext");
    ns->addIncoming(nNext, nAdvBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(nProbeLpBB)));

    builder->SetInsertPoint(nWriteBB);
    {   auto* st = builder->CreateAlignedStore(hashVal, nHashPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_); }
    llvm::Value* nKeyPtr = builder->CreateInBoundsGEP(i64Ty, newBuf,
        builder->CreateAdd(nEntryOff, llvm::ConstantInt::get(i64Ty, 1), "nkeyoff", /*HasNUW=*/true, /*HasNSW=*/true), "nkeyptr");
    {   auto* st = builder->CreateAlignedStore(keyArg, nKeyPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_); }
    llvm::Value* nValPtr = builder->CreateInBoundsGEP(i64Ty, newBuf,
        builder->CreateAdd(nEntryOff, llvm::ConstantInt::get(i64Ty, 2), "nvaloff", /*HasNUW=*/true, /*HasNSW=*/true), "nvalptr");
    {   auto* st = builder->CreateAlignedStore(valArg, nValPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_); }
    builder->CreateBr(doneBB);

    // Done: return map pointer
    builder->SetInsertPoint(doneBB);
    llvm::PHINode* result = builder->CreatePHI(ptrTy, 3, "result");
    result->addIncoming(mapArg, matchBB);   // updated in place
    result->addIncoming(mapArg, insertBB);  // inserted without grow
    result->addIncoming(newBuf, nWriteBB);  // grew and inserted
    builder->CreateRet(result);

    builder->restoreIP(savedIP);
    return fn;
}

// __omsc_hmap_get(map: ptr, key: i64, def: i64) -> i64
llvm::Function* CodeGenerator::getOrEmitHashMapGet() {
    if (auto* fn = module->getFunction("__omsc_hmap_get"))
        return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = getDefaultType();
    auto* fnTy = llvm::FunctionType::get(i64Ty, {ptrTy, i64Ty, i64Ty}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__omsc_hmap_get", module.get());
    setHashMapFnAttrs(fn);
    // OmScript ownership: map is the sole reference; get is read-only.
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->getArg(0)->setName("map");
    fn->getArg(1)->setName("key");
    fn->getArg(2)->setName("def");

    auto savedIP = builder->saveIP();

    auto* entryBB = llvm::BasicBlock::Create(*context, "entry", fn);
    auto* probeBB = llvm::BasicBlock::Create(*context, "probe", fn);
    auto* checkBB = llvm::BasicBlock::Create(*context, "check", fn);
    auto* foundBB = llvm::BasicBlock::Create(*context, "found", fn);
    auto* nextBB  = llvm::BasicBlock::Create(*context, "next", fn);
    auto* notfoundBB = llvm::BasicBlock::Create(*context, "notfound", fn);

    builder->SetInsertPoint(entryBB);
    llvm::Value* mapArg = fn->getArg(0);
    llvm::Value* keyArg = fn->getArg(1);
    llvm::Value* defArg = fn->getArg(2);

    // Rotate-Accumulate (RA) hash
    llvm::Value* hashVal = emitKeyHash(keyArg);

    auto* capLoad = builder->CreateAlignedLoad(i64Ty, mapArg, llvm::MaybeAlign(8), "cap");
    // Capacity is invariant during a read-only GET — let LLVM hoist/CSE it.
    capLoad->setMetadata(llvm::LLVMContext::MD_invariant_load,
                         llvm::MDNode::get(*context, {}));
    capLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    llvm::Value* cap = capLoad;
    // Capacity is always a power of 2 >= 8.
    {
        llvm::Metadata* rangeMD[] = {
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 8)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 1ULL << 62))};
        capLoad->setMetadata(llvm::LLVMContext::MD_range,
                             llvm::MDNode::get(*context, rangeMD));
        llvm::Value* capM1 = builder->CreateSub(cap, llvm::ConstantInt::get(i64Ty, 1));
        llvm::Value* isPow2 = builder->CreateICmpEQ(
            builder->CreateAnd(cap, capM1), llvm::ConstantInt::get(i64Ty, 0), "cap.pow2");
        llvm::Function* assumeFn = OMSC_GET_INTRINSIC(
            module.get(), llvm::Intrinsic::assume, {});
        builder->CreateCall(assumeFn, {isPow2});
    }
    llvm::Value* mask = builder->CreateSub(cap, llvm::ConstantInt::get(i64Ty, 1), "mask", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* startSlot = builder->CreateAnd(hashVal, mask, "startslot");
    builder->CreateBr(probeBB);

    // Probe loop
    builder->SetInsertPoint(probeBB);
    llvm::PHINode* slot = builder->CreatePHI(i64Ty, 2, "slot");
    slot->addIncoming(startSlot, entryBB);

    llvm::Value* three = llvm::ConstantInt::get(i64Ty, 3);
    llvm::Value* entryOff = builder->CreateAdd(
        builder->CreateMul(slot, three, "slot3", /*HasNUW=*/true, /*HasNSW=*/true), llvm::ConstantInt::get(i64Ty, 2), "entryoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* hashPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, entryOff, "hashptr");
    llvm::Value* slotHash = builder->CreateAlignedLoad(i64Ty, hashPtr, llvm::MaybeAlign(8), "slothash");
    llvm::cast<llvm::Instruction>(slotHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    // Empty => not found
    llvm::Value* isEmpty = builder->CreateICmpEQ(slotHash, llvm::ConstantInt::get(i64Ty, 0), "isempty");
    builder->CreateCondBr(isEmpty, notfoundBB, checkBB);

    // Check: tombstone or occupied?
    builder->SetInsertPoint(checkBB);
    llvm::Value* isTomb = builder->CreateICmpEQ(slotHash, llvm::ConstantInt::get(i64Ty, 1), "istomb");
    // If tombstone, skip to next; otherwise compare hashes first
    auto* hashCmpBB = llvm::BasicBlock::Create(*context, "hashcmp", fn);
    builder->CreateCondBr(isTomb, nextBB, hashCmpBB);

    // Hash pre-filter: compare stored hash vs computed hash before loading key.
    // Avoids an expensive key load (potential cache miss) on hash mismatch.
    builder->SetInsertPoint(hashCmpBB);
    auto* checkKeyBB = llvm::BasicBlock::Create(*context, "checkkey", fn);
    llvm::Value* hashMatch = builder->CreateICmpEQ(slotHash, hashVal, "hashmatch");
    {   auto* w = llvm::MDBuilder(*context).createBranchWeights(1, 4);
        builder->CreateCondBr(hashMatch, checkKeyBB, nextBB, w); }

    builder->SetInsertPoint(checkKeyBB);
    llvm::Value* keyOff = builder->CreateAdd(entryOff, llvm::ConstantInt::get(i64Ty, 1), "keyoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* eKeyPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, keyOff, "ekeyptr");
    llvm::Value* eKey = builder->CreateAlignedLoad(i64Ty, eKeyPtr, llvm::MaybeAlign(8), "ekey");
    llvm::cast<llvm::Instruction>(eKey)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
    llvm::Value* keyMatch = builder->CreateICmpEQ(eKey, keyArg, "keymatch");
    {   auto* w = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(keyMatch, foundBB, nextBB, w); }

    // Found: load value
    builder->SetInsertPoint(foundBB);
    llvm::Value* valOff = builder->CreateAdd(entryOff, llvm::ConstantInt::get(i64Ty, 2), "valoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* eValPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, valOff, "evalptr");
    llvm::Value* val = builder->CreateAlignedLoad(i64Ty, eValPtr, llvm::MaybeAlign(8), "val");
    llvm::cast<llvm::Instruction>(val)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_);
    builder->CreateRet(val);

    // Next: advance slot (both tombstone and no-match paths funnel here)
    builder->SetInsertPoint(nextBB);
    llvm::Value* nextSlot = builder->CreateAnd(
        builder->CreateAdd(slot, llvm::ConstantInt::get(i64Ty, 1), "slot1", /*HasNUW=*/true, /*HasNSW=*/true),
        mask, "nextslot");
    slot->addIncoming(nextSlot, nextBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(probeBB)));

    // Not found: return default
    builder->SetInsertPoint(notfoundBB);
    builder->CreateRet(defArg);

    builder->restoreIP(savedIP);
    return fn;
}

// __omsc_hmap_has(map: ptr, key: i64) -> i64 (0 or 1)
llvm::Function* CodeGenerator::getOrEmitHashMapHas() {
    if (auto* fn = module->getFunction("__omsc_hmap_has"))
        return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = getDefaultType();
    auto* fnTy = llvm::FunctionType::get(i64Ty, {ptrTy, i64Ty}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__omsc_hmap_has", module.get());
    setHashMapFnAttrs(fn);
    // OmScript ownership: map is the sole reference; has is read-only.
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->getArg(0)->setName("map");
    fn->getArg(1)->setName("key");

    auto savedIP = builder->saveIP();

    auto* entryBB    = llvm::BasicBlock::Create(*context, "entry", fn);
    auto* probeBB    = llvm::BasicBlock::Create(*context, "probe", fn);
    auto* checkBB    = llvm::BasicBlock::Create(*context, "check", fn);
    auto* checkKeyBB = llvm::BasicBlock::Create(*context, "checkkey", fn);
    auto* foundBB    = llvm::BasicBlock::Create(*context, "found", fn);
    auto* nextBB     = llvm::BasicBlock::Create(*context, "next", fn);
    auto* notfoundBB = llvm::BasicBlock::Create(*context, "notfound", fn);

    builder->SetInsertPoint(entryBB);
    llvm::Value* mapArg = fn->getArg(0);
    llvm::Value* keyArg = fn->getArg(1);

    // Rotate-Accumulate (RA) hash
    llvm::Value* hashVal = emitKeyHash(keyArg);

    auto* capLoad_has = builder->CreateAlignedLoad(i64Ty, mapArg, llvm::MaybeAlign(8), "cap");
    // Capacity is invariant during a read-only HAS — let LLVM hoist/CSE it.
    capLoad_has->setMetadata(llvm::LLVMContext::MD_invariant_load,
                             llvm::MDNode::get(*context, {}));
    capLoad_has->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    llvm::Value* cap = capLoad_has;
    // Capacity is always a power of 2 >= 8.
    {
        llvm::Metadata* rangeMD[] = {
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 8)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 1ULL << 62))};
        capLoad_has->setMetadata(llvm::LLVMContext::MD_range,
                             llvm::MDNode::get(*context, rangeMD));
        llvm::Value* capM1 = builder->CreateSub(cap, llvm::ConstantInt::get(i64Ty, 1));
        llvm::Value* isPow2 = builder->CreateICmpEQ(
            builder->CreateAnd(cap, capM1), llvm::ConstantInt::get(i64Ty, 0), "cap.pow2");
        llvm::Function* assumeFn = OMSC_GET_INTRINSIC(
            module.get(), llvm::Intrinsic::assume, {});
        builder->CreateCall(assumeFn, {isPow2});
    }
    llvm::Value* mask = builder->CreateSub(cap, llvm::ConstantInt::get(i64Ty, 1), "mask", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* startSlot = builder->CreateAnd(hashVal, mask, "startslot");
    builder->CreateBr(probeBB);

    builder->SetInsertPoint(probeBB);
    llvm::PHINode* slot = builder->CreatePHI(i64Ty, 2, "slot");
    slot->addIncoming(startSlot, entryBB);

    llvm::Value* three = llvm::ConstantInt::get(i64Ty, 3);
    llvm::Value* entryOff = builder->CreateAdd(
        builder->CreateMul(slot, three, "slot3", /*HasNUW=*/true, /*HasNSW=*/true), llvm::ConstantInt::get(i64Ty, 2), "entryoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* hashPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, entryOff, "hashptr");
    llvm::Value* slotHash = builder->CreateAlignedLoad(i64Ty, hashPtr, llvm::MaybeAlign(8), "slothash");
    llvm::cast<llvm::Instruction>(slotHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    llvm::Value* isEmpty = builder->CreateICmpEQ(slotHash, llvm::ConstantInt::get(i64Ty, 0), "isempty");
    builder->CreateCondBr(isEmpty, notfoundBB, checkBB);

    builder->SetInsertPoint(checkBB);
    llvm::Value* isTomb = builder->CreateICmpEQ(slotHash, llvm::ConstantInt::get(i64Ty, 1), "istomb");
    auto* hashCmpBB = llvm::BasicBlock::Create(*context, "hashcmp", fn);
    builder->CreateCondBr(isTomb, nextBB, hashCmpBB);

    // Hash pre-filter: compare stored hash vs computed hash before loading key.
    builder->SetInsertPoint(hashCmpBB);
    llvm::Value* hashMatch = builder->CreateICmpEQ(slotHash, hashVal, "hashmatch");
    {   auto* w = llvm::MDBuilder(*context).createBranchWeights(1, 4);
        builder->CreateCondBr(hashMatch, checkKeyBB, nextBB, w); }

    builder->SetInsertPoint(checkKeyBB);
    llvm::Value* keyOff = builder->CreateAdd(entryOff, llvm::ConstantInt::get(i64Ty, 1), "keyoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* eKeyPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, keyOff, "ekeyptr");
    llvm::Value* eKey = builder->CreateAlignedLoad(i64Ty, eKeyPtr, llvm::MaybeAlign(8), "ekey");
    llvm::cast<llvm::Instruction>(eKey)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
    llvm::Value* keyMatch = builder->CreateICmpEQ(eKey, keyArg, "keymatch");
    {   auto* w = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(keyMatch, foundBB, nextBB, w); }

    builder->SetInsertPoint(foundBB);
    builder->CreateRet(llvm::ConstantInt::get(i64Ty, 1));

    builder->SetInsertPoint(nextBB);
    llvm::Value* nextSlot = builder->CreateAnd(
        builder->CreateAdd(slot, llvm::ConstantInt::get(i64Ty, 1), "slot1", /*HasNUW=*/true, /*HasNSW=*/true),
        mask, "nextslot");
    slot->addIncoming(nextSlot, nextBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(probeBB)));

    builder->SetInsertPoint(notfoundBB);
    builder->CreateRet(llvm::ConstantInt::get(i64Ty, 0));

    builder->restoreIP(savedIP);
    return fn;
}

// __omsc_hmap_remove(map: ptr, key: i64) -> ptr
//   Mark the entry as tombstone (hash=1).  Returns the same map pointer.
llvm::Function* CodeGenerator::getOrEmitHashMapRemove() {
    if (auto* fn = module->getFunction("__omsc_hmap_remove"))
        return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = getDefaultType();
    auto* fnTy = llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__omsc_hmap_remove", module.get());
    setHashMapFnAttrs(fn);
    // OmScript ownership: map is the sole reference.
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    // Returns the same map pointer (unique ownership).
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull);
    fn->getArg(0)->setName("map");
    fn->getArg(1)->setName("key");

    auto savedIP = builder->saveIP();

    auto* entryBB    = llvm::BasicBlock::Create(*context, "entry", fn);
    auto* probeBB    = llvm::BasicBlock::Create(*context, "probe", fn);
    auto* checkBB    = llvm::BasicBlock::Create(*context, "check", fn);
    auto* checkKeyBB = llvm::BasicBlock::Create(*context, "checkkey", fn);
    auto* foundBB    = llvm::BasicBlock::Create(*context, "found", fn);
    auto* nextBB     = llvm::BasicBlock::Create(*context, "next", fn);
    auto* doneBB     = llvm::BasicBlock::Create(*context, "done", fn);

    builder->SetInsertPoint(entryBB);
    llvm::Value* mapArg = fn->getArg(0);
    llvm::Value* keyArg = fn->getArg(1);

    // Rotate-Accumulate (RA) hash
    llvm::Value* hashVal = emitKeyHash(keyArg);

    auto* capLoad_rm = builder->CreateAlignedLoad(i64Ty, mapArg, llvm::MaybeAlign(8), "cap");
    capLoad_rm->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    llvm::Value* cap = capLoad_rm;
    // Capacity is always a power of 2 >= 8.
    {
        llvm::Metadata* rangeMD[] = {
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 8)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 1ULL << 62))};
        capLoad_rm->setMetadata(llvm::LLVMContext::MD_range,
                             llvm::MDNode::get(*context, rangeMD));
        llvm::Value* capM1 = builder->CreateSub(cap, llvm::ConstantInt::get(i64Ty, 1));
        llvm::Value* isPow2 = builder->CreateICmpEQ(
            builder->CreateAnd(cap, capM1), llvm::ConstantInt::get(i64Ty, 0), "cap.pow2");
        llvm::Function* assumeFn = OMSC_GET_INTRINSIC(
            module.get(), llvm::Intrinsic::assume, {});
        builder->CreateCall(assumeFn, {isPow2});
    }
    llvm::Value* mask = builder->CreateSub(cap, llvm::ConstantInt::get(i64Ty, 1), "mask", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* startSlot = builder->CreateAnd(hashVal, mask, "startslot");
    builder->CreateBr(probeBB);

    builder->SetInsertPoint(probeBB);
    llvm::PHINode* slot = builder->CreatePHI(i64Ty, 2, "slot");
    slot->addIncoming(startSlot, entryBB);

    llvm::Value* three = llvm::ConstantInt::get(i64Ty, 3);
    llvm::Value* entryOff = builder->CreateAdd(
        builder->CreateMul(slot, three, "slot3", /*HasNUW=*/true, /*HasNSW=*/true), llvm::ConstantInt::get(i64Ty, 2), "entryoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* hashPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, entryOff, "hashptr");
    llvm::Value* slotHash = builder->CreateAlignedLoad(i64Ty, hashPtr, llvm::MaybeAlign(8), "slothash");
    llvm::cast<llvm::Instruction>(slotHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    llvm::Value* isEmpty = builder->CreateICmpEQ(slotHash, llvm::ConstantInt::get(i64Ty, 0), "isempty");
    builder->CreateCondBr(isEmpty, doneBB, checkBB);

    builder->SetInsertPoint(checkBB);
    llvm::Value* isTomb = builder->CreateICmpEQ(slotHash, llvm::ConstantInt::get(i64Ty, 1), "istomb");
    auto* hashCmpBB = llvm::BasicBlock::Create(*context, "hashcmp", fn);
    builder->CreateCondBr(isTomb, nextBB, hashCmpBB);

    // Hash pre-filter: compare stored hash vs computed hash before loading key.
    builder->SetInsertPoint(hashCmpBB);
    llvm::Value* hashMatch = builder->CreateICmpEQ(slotHash, hashVal, "hashmatch");
    {   auto* w = llvm::MDBuilder(*context).createBranchWeights(1, 4);
        builder->CreateCondBr(hashMatch, checkKeyBB, nextBB, w); }

    builder->SetInsertPoint(checkKeyBB);
    llvm::Value* keyOff = builder->CreateAdd(entryOff, llvm::ConstantInt::get(i64Ty, 1), "keyoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* eKeyPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, keyOff, "ekeyptr");
    llvm::Value* eKey = builder->CreateAlignedLoad(i64Ty, eKeyPtr, llvm::MaybeAlign(8), "ekey");
    llvm::cast<llvm::Instruction>(eKey)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
    llvm::Value* keyMatch = builder->CreateICmpEQ(eKey, keyArg, "keymatch");
    {   auto* w = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(keyMatch, foundBB, nextBB, w); }

    // Found: mark as tombstone, decrement size
    builder->SetInsertPoint(foundBB);
    {   auto* st = builder->CreateAlignedStore(llvm::ConstantInt::get(i64Ty, 1), hashPtr, llvm::MaybeAlign(8));
        st->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_); }
    llvm::Value* sizePtr = builder->CreateInBoundsGEP(i64Ty, mapArg, llvm::ConstantInt::get(i64Ty, 1));
    auto* sizeLoad_rm = builder->CreateAlignedLoad(i64Ty, sizePtr, llvm::MaybeAlign(8), "size");
    sizeLoad_rm->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    llvm::Value* size = sizeLoad_rm;
    llvm::Value* newSize = builder->CreateSub(size, llvm::ConstantInt::get(i64Ty, 1), "newsize", /*HasNUW=*/true, /*HasNSW=*/true);
    auto* sizeStore_rm = builder->CreateAlignedStore(newSize, sizePtr, llvm::MaybeAlign(8));
    sizeStore_rm->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    builder->CreateBr(doneBB);

    builder->SetInsertPoint(nextBB);
    llvm::Value* nextSlot = builder->CreateAnd(
        builder->CreateAdd(slot, llvm::ConstantInt::get(i64Ty, 1), "slot1", /*HasNUW=*/true, /*HasNSW=*/true),
        mask, "nextslot");
    slot->addIncoming(nextSlot, nextBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(probeBB)));

    // Done: return same map pointer (mutated in place)
    builder->SetInsertPoint(doneBB);
    builder->CreateRet(mapArg);

    builder->restoreIP(savedIP);
    return fn;
}

// __omsc_hmap_keys(map: ptr) -> ptr (array)
llvm::Function* CodeGenerator::getOrEmitHashMapKeys() {
    if (auto* fn = module->getFunction("__omsc_hmap_keys"))
        return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = getDefaultType();
    auto* fnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__omsc_hmap_keys", module.get());
    setHashMapFnAttrs(fn);
    // OmScript ownership: map is the sole reference; keys only reads the map.
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    // Returns a fresh malloc'd array — always unique and non-null.
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull);
    fn->getArg(0)->setName("map");

    auto savedIP = builder->saveIP();
    auto* entryBB = llvm::BasicBlock::Create(*context, "entry", fn);
    auto* loopBB  = llvm::BasicBlock::Create(*context, "loop", fn);
    auto* bodyBB  = llvm::BasicBlock::Create(*context, "body", fn);
    auto* storeBB = llvm::BasicBlock::Create(*context, "store", fn);
    auto* nextBB  = llvm::BasicBlock::Create(*context, "next", fn);
    auto* doneBB  = llvm::BasicBlock::Create(*context, "done", fn);

    builder->SetInsertPoint(entryBB);
    llvm::Value* mapArg = fn->getArg(0);
    auto* capLoad_keys = builder->CreateAlignedLoad(i64Ty, mapArg, llvm::MaybeAlign(8), "cap");
    capLoad_keys->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    llvm::Value* cap = capLoad_keys;
    llvm::Value* sizePtr = builder->CreateInBoundsGEP(i64Ty, mapArg, llvm::ConstantInt::get(i64Ty, 1));
    auto* sizeLoad_keys = builder->CreateAlignedLoad(i64Ty, sizePtr, llvm::MaybeAlign(8), "size");
    sizeLoad_keys->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    llvm::Value* size = sizeLoad_keys;

    // Allocate output array: (size + 1) * 8
    llvm::Value* eight = llvm::ConstantInt::get(i64Ty, 8);
    llvm::Value* arrSlots = builder->CreateAdd(size, llvm::ConstantInt::get(i64Ty, 1), "arrslots", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* arrBytes = builder->CreateMul(arrSlots, eight, "arrbytes", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {arrBytes}, "buf");
    llvm::cast<llvm::CallInst>(buf)->addRetAttr(
        llvm::Attribute::getWithDereferenceableBytes(*context, 8));
    builder->CreateAlignedStore(size, buf, llvm::MaybeAlign(8));
    builder->CreateBr(loopBB);

    // Loop over hash table slots
    builder->SetInsertPoint(loopBB);
    llvm::PHINode* i = builder->CreatePHI(i64Ty, 2, "i");
    i->addIncoming(llvm::ConstantInt::get(i64Ty, 0), entryBB);
    llvm::PHINode* writeIdx = builder->CreatePHI(i64Ty, 2, "widx");
    writeIdx->addIncoming(llvm::ConstantInt::get(i64Ty, 0), entryBB);
    llvm::Value* cond = builder->CreateICmpULT(i, cap, "cond");
    builder->CreateCondBr(cond, bodyBB, doneBB);

    builder->SetInsertPoint(bodyBB);
    llvm::Value* three = llvm::ConstantInt::get(i64Ty, 3);
    llvm::Value* off = builder->CreateAdd(
        builder->CreateMul(i, three, "i3", /*HasNUW=*/true, /*HasNSW=*/true), llvm::ConstantInt::get(i64Ty, 2), "off", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* hashPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, off, "hashptr");
    llvm::Value* slotHash = builder->CreateAlignedLoad(i64Ty, hashPtr, llvm::MaybeAlign(8), "slothash");
    llvm::cast<llvm::Instruction>(slotHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    llvm::Value* isActive = builder->CreateICmpUGE(slotHash, llvm::ConstantInt::get(i64Ty, 2), "isactive");
    builder->CreateCondBr(isActive, storeBB, nextBB);

    builder->SetInsertPoint(storeBB);
    llvm::Value* keyOff = builder->CreateAdd(off, llvm::ConstantInt::get(i64Ty, 1), "keyoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* keyPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, keyOff, "keyptr");
    llvm::Value* key = builder->CreateAlignedLoad(i64Ty, keyPtr, llvm::MaybeAlign(8), "key");
    llvm::cast<llvm::Instruction>(key)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapKey_);
    llvm::Value* arrOff = builder->CreateAdd(writeIdx, llvm::ConstantInt::get(i64Ty, 1), "arroff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* arrPtr = builder->CreateInBoundsGEP(i64Ty, buf, arrOff, "arrptr");
    builder->CreateAlignedStore(key, arrPtr, llvm::MaybeAlign(8));
    llvm::Value* newWIdx = builder->CreateAdd(writeIdx, llvm::ConstantInt::get(i64Ty, 1), "newidx", /*HasNUW=*/true, /*HasNSW=*/true);
    builder->CreateBr(nextBB);

    builder->SetInsertPoint(nextBB);
    llvm::PHINode* wPhi = builder->CreatePHI(i64Ty, 2, "wphi");
    wPhi->addIncoming(writeIdx, bodyBB);   // not active: unchanged
    wPhi->addIncoming(newWIdx, storeBB);   // active: incremented
    llvm::Value* iNext = builder->CreateAdd(i, llvm::ConstantInt::get(i64Ty, 1), "inext", /*HasNUW=*/true, /*HasNSW=*/true);
    i->addIncoming(iNext, nextBB);
    writeIdx->addIncoming(wPhi, nextBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

    builder->SetInsertPoint(doneBB);
    builder->CreateRet(buf);

    builder->restoreIP(savedIP);
    return fn;
}

// __omsc_hmap_values(map: ptr) -> ptr (array)
llvm::Function* CodeGenerator::getOrEmitHashMapValues() {
    if (auto* fn = module->getFunction("__omsc_hmap_values"))
        return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = getDefaultType();
    auto* fnTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__omsc_hmap_values", module.get());
    setHashMapFnAttrs(fn);
    // OmScript ownership: map is the sole reference; values only reads the map.
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    // Returns a fresh malloc'd array — always unique and non-null.
    fn->addRetAttr(llvm::Attribute::NoAlias);
    fn->addRetAttr(llvm::Attribute::NonNull);
    fn->getArg(0)->setName("map");

    auto savedIP = builder->saveIP();
    auto* entryBB = llvm::BasicBlock::Create(*context, "entry", fn);
    auto* loopBB  = llvm::BasicBlock::Create(*context, "loop", fn);
    auto* bodyBB  = llvm::BasicBlock::Create(*context, "body", fn);
    auto* storeBB = llvm::BasicBlock::Create(*context, "store", fn);
    auto* nextBB  = llvm::BasicBlock::Create(*context, "next", fn);
    auto* doneBB  = llvm::BasicBlock::Create(*context, "done", fn);

    builder->SetInsertPoint(entryBB);
    llvm::Value* mapArg = fn->getArg(0);
    auto* capLoad_vals = builder->CreateAlignedLoad(i64Ty, mapArg, llvm::MaybeAlign(8), "cap");
    capLoad_vals->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    llvm::Value* cap = capLoad_vals;
    llvm::Value* sizePtr = builder->CreateInBoundsGEP(i64Ty, mapArg, llvm::ConstantInt::get(i64Ty, 1));
    auto* sizeLoad_vals = builder->CreateAlignedLoad(i64Ty, sizePtr, llvm::MaybeAlign(8), "size");
    sizeLoad_vals->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    llvm::Value* size = sizeLoad_vals;

    llvm::Value* eight = llvm::ConstantInt::get(i64Ty, 8);
    llvm::Value* arrSlots = builder->CreateAdd(size, llvm::ConstantInt::get(i64Ty, 1), "arrslots", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* arrBytes = builder->CreateMul(arrSlots, eight, "arrbytes", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {arrBytes}, "buf");
    llvm::cast<llvm::CallInst>(buf)->addRetAttr(
        llvm::Attribute::getWithDereferenceableBytes(*context, 8));
    builder->CreateAlignedStore(size, buf, llvm::MaybeAlign(8));
    builder->CreateBr(loopBB);

    builder->SetInsertPoint(loopBB);
    llvm::PHINode* i = builder->CreatePHI(i64Ty, 2, "i");
    i->addIncoming(llvm::ConstantInt::get(i64Ty, 0), entryBB);
    llvm::PHINode* writeIdx = builder->CreatePHI(i64Ty, 2, "widx");
    writeIdx->addIncoming(llvm::ConstantInt::get(i64Ty, 0), entryBB);
    llvm::Value* cond = builder->CreateICmpULT(i, cap, "cond");
    builder->CreateCondBr(cond, bodyBB, doneBB);

    builder->SetInsertPoint(bodyBB);
    llvm::Value* three = llvm::ConstantInt::get(i64Ty, 3);
    llvm::Value* off = builder->CreateAdd(
        builder->CreateMul(i, three, "i3", /*HasNUW=*/true, /*HasNSW=*/true), llvm::ConstantInt::get(i64Ty, 2), "off", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* hashPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, off, "hashptr");
    llvm::Value* slotHash = builder->CreateAlignedLoad(i64Ty, hashPtr, llvm::MaybeAlign(8), "slothash");
    llvm::cast<llvm::Instruction>(slotHash)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapHash_);
    llvm::Value* isActive = builder->CreateICmpUGE(slotHash, llvm::ConstantInt::get(i64Ty, 2), "isactive");
    builder->CreateCondBr(isActive, storeBB, nextBB);

    builder->SetInsertPoint(storeBB);
    llvm::Value* valOff = builder->CreateAdd(off, llvm::ConstantInt::get(i64Ty, 2), "valoff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* valPtr = builder->CreateInBoundsGEP(i64Ty, mapArg, valOff, "valptr");
    llvm::Value* val = builder->CreateAlignedLoad(i64Ty, valPtr, llvm::MaybeAlign(8), "val");
    llvm::cast<llvm::Instruction>(val)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapVal_);
    llvm::Value* arrOff = builder->CreateAdd(writeIdx, llvm::ConstantInt::get(i64Ty, 1), "arroff", /*HasNUW=*/true, /*HasNSW=*/true);
    llvm::Value* arrPtr = builder->CreateInBoundsGEP(i64Ty, buf, arrOff, "arrptr");
    builder->CreateAlignedStore(val, arrPtr, llvm::MaybeAlign(8));
    llvm::Value* newWIdx = builder->CreateAdd(writeIdx, llvm::ConstantInt::get(i64Ty, 1), "newidx", /*HasNUW=*/true, /*HasNSW=*/true);
    builder->CreateBr(nextBB);

    builder->SetInsertPoint(nextBB);
    llvm::PHINode* wPhi = builder->CreatePHI(i64Ty, 2, "wphi");
    wPhi->addIncoming(writeIdx, bodyBB);
    wPhi->addIncoming(newWIdx, storeBB);
    llvm::Value* iNext = builder->CreateAdd(i, llvm::ConstantInt::get(i64Ty, 1), "inext", /*HasNUW=*/true, /*HasNSW=*/true);
    i->addIncoming(iNext, nextBB);
    writeIdx->addIncoming(wPhi, nextBB);
    attachLoopMetadata(llvm::cast<llvm::BranchInst>(builder->CreateBr(loopBB)));

    builder->SetInsertPoint(doneBB);
    builder->CreateRet(buf);

    builder->restoreIP(savedIP);
    return fn;
}

// __omsc_hmap_size(map: ptr) -> i64
llvm::Function* CodeGenerator::getOrEmitHashMapSize() {
    if (auto* fn = module->getFunction("__omsc_hmap_size"))
        return fn;

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    auto* i64Ty = getDefaultType();
    auto* fnTy = llvm::FunctionType::get(i64Ty, {ptrTy}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__omsc_hmap_size", module.get());
    setHashMapFnAttrs(fn);
    // OmScript ownership: map is the sole reference; size only reads slot[1].
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    fn->addParamAttr(0, llvm::Attribute::NonNull);
    fn->addParamAttr(0, llvm::Attribute::ReadOnly);
    fn->getArg(0)->setName("map");

    auto savedIP = builder->saveIP();
    auto* entryBB = llvm::BasicBlock::Create(*context, "entry", fn);
    builder->SetInsertPoint(entryBB);
    llvm::Value* sizePtr = builder->CreateInBoundsGEP(i64Ty, fn->getArg(0),
        llvm::ConstantInt::get(i64Ty, 1), "sizeptr");
    auto* sizeLoad_sz = builder->CreateAlignedLoad(i64Ty, sizePtr, llvm::MaybeAlign(8), "size");
    sizeLoad_sz->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaMapMeta_);
    // Size is always non-negative.
    {   llvm::Metadata* rangeMD[] = {
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 0)),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Ty, 1ULL << 62))};
        sizeLoad_sz->setMetadata(llvm::LLVMContext::MD_range,
                                 llvm::MDNode::get(*context, rangeMD));
    }
    builder->CreateRet(sizeLoad_sz);

    builder->restoreIP(savedIP);
    return fn;
}

// ---------------------------------------------------------------------------

bool CodeGenerator::isPreAnalysisStringExpr(Expression* expr, const std::unordered_set<size_t>& paramStringIndices,
                                            const FunctionDecl* func) const {
    if (!expr)
        return false;
    if (auto* lit = dynamic_cast<LiteralExpr*>(expr))
        return lit->literalType == LiteralExpr::LiteralType::STRING;
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
        if (func) {
            for (size_t i = 0; i < func->parameters.size(); i++) {
                if (func->parameters[i].name == id->name && paramStringIndices.count(i))
                    return true;
            }
        }
        return false;
    }
    if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
        if (bin->op == "+" || bin->op == "*")
            return isPreAnalysisStringExpr(bin->left.get(), paramStringIndices, func) ||
                   isPreAnalysisStringExpr(bin->right.get(), paramStringIndices, func);
        return false;
    }
    if (auto* tern = dynamic_cast<TernaryExpr*>(expr)) {
        return isPreAnalysisStringExpr(tern->thenExpr.get(), paramStringIndices, func) ||
               isPreAnalysisStringExpr(tern->elseExpr.get(), paramStringIndices, func);
    }
    if (auto* call = dynamic_cast<CallExpr*>(expr))
        return stringReturningFunctions_.count(call->callee) > 0;
    return false;
}

bool CodeGenerator::scanStmtForStringReturns(Statement* stmt, const std::unordered_set<size_t>& paramStringIndices,
                                             const FunctionDecl* func) const {
    if (!stmt)
        return false;
    if (auto* ret = dynamic_cast<ReturnStmt*>(stmt))
        return ret->value && isPreAnalysisStringExpr(ret->value.get(), paramStringIndices, func);
    if (auto* block = dynamic_cast<BlockStmt*>(stmt)) {
        for (auto& s : block->statements)
            if (scanStmtForStringReturns(s.get(), paramStringIndices, func))
                return true;
    }
    if (auto* ifS = dynamic_cast<IfStmt*>(stmt)) {
        return scanStmtForStringReturns(ifS->thenBranch.get(), paramStringIndices, func) ||
               scanStmtForStringReturns(ifS->elseBranch.get(), paramStringIndices, func);
    }
    if (auto* whileS = dynamic_cast<WhileStmt*>(stmt))
        return scanStmtForStringReturns(whileS->body.get(), paramStringIndices, func);
    if (auto* doS = dynamic_cast<DoWhileStmt*>(stmt))
        return scanStmtForStringReturns(doS->body.get(), paramStringIndices, func);
    if (auto* forS = dynamic_cast<ForStmt*>(stmt))
        return scanStmtForStringReturns(forS->body.get(), paramStringIndices, func);
    if (auto* forEachS = dynamic_cast<ForEachStmt*>(stmt))
        return scanStmtForStringReturns(forEachS->body.get(), paramStringIndices, func);
    return false;
}

void CodeGenerator::scanStmtForStringCalls(Statement* stmt) {
    if (!stmt)
        return;
    auto scanExpr = [this](auto& self, Expression* expr) -> void {
        if (!expr)
            return;
        if (auto* call = dynamic_cast<CallExpr*>(expr)) {
            for (size_t i = 0; i < call->arguments.size(); i++) {
                if (isPreAnalysisStringExpr(call->arguments[i].get(), {}, nullptr))
                    funcParamStringTypes_[call->callee].insert(i);
            }
            for (auto& arg : call->arguments)
                self(self, arg.get());
        }
        if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
            self(self, bin->left.get());
            self(self, bin->right.get());
        }
        if (auto* assign = dynamic_cast<AssignExpr*>(expr))
            self(self, assign->value.get());
        if (auto* tern = dynamic_cast<TernaryExpr*>(expr)) {
            self(self, tern->condition.get());
            self(self, tern->thenExpr.get());
            self(self, tern->elseExpr.get());
        }
    };
    auto scanStmt = [&](auto& self, Statement* s) -> void {
        if (!s)
            return;
        if (auto* block = dynamic_cast<BlockStmt*>(s)) {
            for (auto& st : block->statements)
                self(self, st.get());
        } else if (auto* exprS = dynamic_cast<ExprStmt*>(s)) {
            scanExpr(scanExpr, exprS->expression.get());
        } else if (auto* varDecl = dynamic_cast<VarDecl*>(s)) {
            if (varDecl->initializer)
                scanExpr(scanExpr, varDecl->initializer.get());
        } else if (auto* ret = dynamic_cast<ReturnStmt*>(s)) {
            if (ret->value)
                scanExpr(scanExpr, ret->value.get());
        } else if (auto* ifS = dynamic_cast<IfStmt*>(s)) {
            scanExpr(scanExpr, ifS->condition.get());
            self(self, ifS->thenBranch.get());
            self(self, ifS->elseBranch.get());
        } else if (auto* whileS = dynamic_cast<WhileStmt*>(s)) {
            scanExpr(scanExpr, whileS->condition.get());
            self(self, whileS->body.get());
        } else if (auto* doS = dynamic_cast<DoWhileStmt*>(s)) {
            self(self, doS->body.get());
            scanExpr(scanExpr, doS->condition.get());
        } else if (auto* forS = dynamic_cast<ForStmt*>(s)) {
            if (forS->start)
                scanExpr(scanExpr, forS->start.get());
            if (forS->end)
                scanExpr(scanExpr, forS->end.get());
            if (forS->step)
                scanExpr(scanExpr, forS->step.get());
            self(self, forS->body.get());
        } else if (auto* forEachS = dynamic_cast<ForEachStmt*>(s)) {
            scanExpr(scanExpr, forEachS->collection.get());
            self(self, forEachS->body.get());
        }
    };
    scanStmt(scanStmt, stmt);
}

void CodeGenerator::preAnalyzeStringTypes(Program* program) {
    // Seed string type information from explicit type annotations.
    for (auto& func : program->functions) {
        // Seed string-returning functions from return type annotations.
        if (func->returnType == "string") {
            stringReturningFunctions_.insert(func->name);
        }
        // Seed string parameter types from parameter type annotations.
        for (size_t i = 0; i < func->parameters.size(); ++i) {
            if (func->parameters[i].typeName == "string") {
                funcParamStringTypes_[func->name].insert(i);
            }
        }
    }

    // Iteratively propagate string type information until no new facts are learned.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& func : program->functions) {
            // Collect the set of parameter indices known to receive string arguments
            // for this function (from funcParamStringTypes_ populated so far).
            std::unordered_set<size_t> paramStrIdx;
            auto pit = funcParamStringTypes_.find(func->name);
            if (pit != funcParamStringTypes_.end())
                paramStrIdx = pit->second;

            // Check if this function returns a string value.
            if (!stringReturningFunctions_.count(func->name)) {
                if (scanStmtForStringReturns(func->body.get(), paramStrIdx, func.get())) {
                    stringReturningFunctions_.insert(func->name);
                    changed = true;
                }
            }

            // Scan all call sites in this function's body to find which parameters
            size_t prevTotal = 0;
            for (auto& kv : funcParamStringTypes_)
                prevTotal += kv.second.size();
            scanStmtForStringCalls(func->body.get());
            size_t newTotal = 0;
            for (auto& kv : funcParamStringTypes_)
                newTotal += kv.second.size();
            if (newTotal > prevTotal)
                changed = true;
        }
    }
}

// Helper: true if a type annotation string looks like an array type (ends with []).
static bool isArrayAnnotation(const std::string& ann) {
    return ann.size() >= 2 && ann.compare(ann.size() - 2, 2, "[]") == 0;
}

// Helper: true if an expression at the pre-analysis stage looks like it
// produces an array value (annotated [], array literal, array builtins).
static bool isPreAnalysisArrayExpr(Expression* expr,
                                    const llvm::StringSet<>& arrayReturningFunctions,
                                    const std::unordered_map<std::string, std::unordered_set<size_t>>& funcParamArrayTypes,
                                    const FunctionDecl* func) {
    if (!expr) return false;
    if (expr->type == ASTNodeType::ARRAY_EXPR) return true;
    if (expr->type == ASTNodeType::CALL_EXPR) {
        auto* call = static_cast<CallExpr*>(expr);
        // Known array-returning builtins
        static const std::unordered_set<std::string> arrayBuiltins = {
            "array_fill", "array_concat", "array_copy", "array_map",
            "array_filter", "array_slice", "sort", "reverse",
            "str_split", "str_chars", "push", "pop", "unshift", "array_remove"
        };
        if (arrayBuiltins.count(call->callee)) return true;
        if (arrayReturningFunctions.count(call->callee)) return true;
    }
    if (expr->type == ASTNodeType::IDENTIFIER_EXPR && func) {
        auto* id = static_cast<IdentifierExpr*>(expr);
        auto it = funcParamArrayTypes.find(func->name);
        if (it != funcParamArrayTypes.end()) {
            for (size_t i = 0; i < func->parameters.size(); ++i) {
                if (func->parameters[i].name == id->name && it->second.count(i))
                    return true;
            }
        }
    }
    return false;
}

void CodeGenerator::preAnalyzeArrayTypes(Program* program) {
    // Seed from explicit [] type annotations.
    for (auto& func : program->functions) {
        if (isArrayAnnotation(func->returnType)) {
            arrayReturningFunctions_.insert(func->name);
        }
        for (size_t i = 0; i < func->parameters.size(); ++i) {
            if (isArrayAnnotation(func->parameters[i].typeName)) {
                funcParamArrayTypes_[func->name].insert(i);
            }
        }
    }

    // Iterative propagation: scan bodies for array-returning return stmts and
    // call sites that pass array arguments.
    auto scanStmt = [&](auto& self, Statement* s) -> bool {
        if (!s) return false;
        if (auto* ret = dynamic_cast<ReturnStmt*>(s)) {
            return ret->value && isPreAnalysisArrayExpr(
                ret->value.get(), arrayReturningFunctions_, funcParamArrayTypes_, nullptr);
        }
        if (auto* blk = dynamic_cast<BlockStmt*>(s)) {
            for (auto& st : blk->statements)
                if (self(self, st.get())) return true;
        } else if (auto* ifS = dynamic_cast<IfStmt*>(s)) {
            if (self(self, ifS->thenBranch.get())) return true;
            if (self(self, ifS->elseBranch.get())) return true;
        } else if (auto* wh = dynamic_cast<WhileStmt*>(s)) {
            if (self(self, wh->body.get())) return true;
        } else if (auto* dw = dynamic_cast<DoWhileStmt*>(s)) {
            if (self(self, dw->body.get())) return true;
        } else if (auto* forS = dynamic_cast<ForStmt*>(s)) {
            if (self(self, forS->body.get())) return true;
        } else if (auto* fe = dynamic_cast<ForEachStmt*>(s)) {
            if (self(self, fe->body.get())) return true;
        }
        return false;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& func : program->functions) {
            if (!arrayReturningFunctions_.count(func->name)) {
                if (scanStmt(scanStmt, func->body.get())) {
                    arrayReturningFunctions_.insert(func->name);
                    changed = true;
                }
            }
            // Scan call sites to propagate array-ness to callee parameters.
            const std::function<void(Statement*)> scanCallSites = [&](Statement* st) {
                if (!st) return;
                auto scanExpr = [&](auto& se, Expression* e) -> void {
                    if (!e) return;
                    if (e->type == ASTNodeType::CALL_EXPR) {
                        auto* call = static_cast<CallExpr*>(e);
                        for (size_t i = 0; i < call->arguments.size(); ++i) {
                            if (isPreAnalysisArrayExpr(call->arguments[i].get(),
                                                       arrayReturningFunctions_,
                                                       funcParamArrayTypes_, func.get())) {
                                if (funcParamArrayTypes_[call->callee].insert(i).second)
                                    changed = true;
                            }
                            se(se, call->arguments[i].get());
                        }
                    } else if (e->type == ASTNodeType::BINARY_EXPR) {
                        auto* bin = static_cast<BinaryExpr*>(e);
                        se(se, bin->left.get()); se(se, bin->right.get());
                    } else if (e->type == ASTNodeType::UNARY_EXPR) {
                        se(se, static_cast<UnaryExpr*>(e)->operand.get());
                    }
                };
                auto scanSt = [&](auto& ss, Statement* s) -> void {
                    if (!s) return;
                    if (auto* es = dynamic_cast<ExprStmt*>(s))
                        scanExpr(scanExpr, es->expression.get());
                    else if (auto* vd = dynamic_cast<VarDecl*>(s))
                        scanExpr(scanExpr, vd->initializer.get());
                    else if (auto* rs = dynamic_cast<ReturnStmt*>(s))
                        scanExpr(scanExpr, rs->value.get());
                    else if (auto* blk = dynamic_cast<BlockStmt*>(s))
                        for (auto& st2 : blk->statements) ss(ss, st2.get());
                    else if (auto* ifS = dynamic_cast<IfStmt*>(s))
                        { ss(ss, ifS->thenBranch.get()); ss(ss, ifS->elseBranch.get()); }
                    else if (auto* wh = dynamic_cast<WhileStmt*>(s))
                        ss(ss, wh->body.get());
                    else if (auto* dw = dynamic_cast<DoWhileStmt*>(s))
                        ss(ss, dw->body.get());
                    else if (auto* fo = dynamic_cast<ForStmt*>(s))
                        ss(ss, fo->body.get());
                    else if (auto* fe = dynamic_cast<ForEachStmt*>(s))
                        ss(ss, fe->body.get());
                };
                scanSt(scanSt, st);
            };
            scanCallSites(func->body.get());
        }
    }
}

bool CodeGenerator::isStringExpr(Expression* expr) const {
    if (!expr)
        return false;
    if (expr->type == ASTNodeType::LITERAL_EXPR)
        return static_cast<LiteralExpr*>(expr)->literalType == LiteralExpr::LiteralType::STRING;
    if (expr->type == ASTNodeType::IDENTIFIER_EXPR) {
        auto* id = static_cast<IdentifierExpr*>(expr);
        // Check the runtime string-variable tracker first (handles params and
        // vars assigned from string function returns).
        if (stringVars_.count(id->name) > 0)
            return true;
        // Check the LLVM alloca type: a pointer-typed alloca is a string ONLY
        if (arrayVars_.count(id->name) || dictVarNames_.count(id->name)
            || structVars_.count(id->name) || stringArrayVars_.count(id->name)
            || ptrVarNames_.count(id->name)
            || refVarElemTypes_.count(id->name))
            return false;
        auto it = namedValues.find(id->name);
        if (it != namedValues.end() && it->second) {
            auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(it->second);
            if (alloca && alloca->getAllocatedType()->isPointerTy())
                return true;
        }
        return false;
    }
    // arr[i] where arr is a string array → the element is a string.
    if (expr->type == ASTNodeType::INDEX_EXPR)
        return isStringArrayExpr(static_cast<IndexExpr*>(expr)->array.get());
    if (expr->type == ASTNodeType::BINARY_EXPR) {
        auto* bin = static_cast<BinaryExpr*>(expr);
        if (bin->op == "+")
            return isStringExpr(bin->left.get()) || isStringExpr(bin->right.get());
        // str * n  and  n * str  both produce a string
        if (bin->op == "*")
            return isStringExpr(bin->left.get()) || isStringExpr(bin->right.get());
        return false;
    }
    if (expr->type == ASTNodeType::TERNARY_EXPR) {
        auto* tern = static_cast<TernaryExpr*>(expr);
        return isStringExpr(tern->thenExpr.get()) || isStringExpr(tern->elseExpr.get());
    }
    if (expr->type == ASTNodeType::CALL_EXPR) {
        auto* call = static_cast<CallExpr*>(expr);
        // If the callee is known to return an array, it is NOT a string.
        if (arrayReturningFunctions_.count(call->callee)) return false;
        return stringReturningFunctions_.count(call->callee) > 0;
    }
    return false;
}

// Returns true if the expression is an array whose elements are string pointers.
bool CodeGenerator::isStringArrayExpr(Expression* expr) const {
    if (!expr)
        return false;
    // Variable whose name is tracked as a string array.
    if (expr->type == ASTNodeType::IDENTIFIER_EXPR)
        return stringArrayVars_.count(static_cast<IdentifierExpr*>(expr)->name) > 0;
    // Array literal whose elements are all strings.
    if (expr->type == ASTNodeType::ARRAY_EXPR) {
        auto* arr = static_cast<ArrayExpr*>(expr);
        if (arr->elements.empty())
            return false;
        for (const auto& e : arr->elements) {
            if (!isStringExpr(e.get()))
                return false;
        }
        return true;
    }
    // str_split() always returns an array of strings.
    if (expr->type == ASTNodeType::CALL_EXPR) {
        auto* call = static_cast<CallExpr*>(expr);
        if (call->callee == "str_split")
            return true;
        if ((call->callee == "push" || call->callee == "unshift" || call->callee == "array_copy") && !call->arguments.empty())
            return isStringArrayExpr(call->arguments[0].get());
        if (call->callee == "array_concat" && !call->arguments.empty())
            return isStringArrayExpr(call->arguments[0].get());
        return false;
    }
    return false;
}


void CodeGenerator::generate(Program* program) {
    hasOptMaxFunctions = false;
    optMaxFunctions.clear();
    optMaxFunctionConfigs_.clear();
    irInstructionCount_ = 0;
    fileNoAlias_ = program->fileNoAlias;

    // --- DWARF debug info initialization ---
    if (debugMode_) {
        debugBuilder_ = std::make_unique<llvm::DIBuilder>(*module);
        const std::string filename = sourceFilename_.empty() ? "source.om" : sourceFilename_;
        debugFile_ = debugBuilder_->createFile(filename, ".");
        debugCU_ = debugBuilder_->createCompileUnit(llvm::dwarf::DW_LANG_C, debugFile_, "OmScript Compiler",
                                                    optimizationLevel != OptimizationLevel::O0, /*Flags=*/"", /*RV=*/0);
        debugScope_ = debugFile_;
        module->addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
        // On Linux, DWARF4 is the most compatible format.
        module->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
    }

    // Detect preferred SIMD vector width from the target's CPU features.
    {
        std::string cpu, features;
        resolveTargetCPU(cpu, features);
        if (features.find("+avx512f") != std::string::npos) {
            preferredVectorWidth_ = 8;  // 8 × i64 = 512 bits
        } else if (features.find("+avx2") != std::string::npos) {
            preferredVectorWidth_ = 4;  // 4 × i64 = 256 bits
        } else {
            preferredVectorWidth_ = 2;  // 2 × i64 = 128 bits (SSE2 / NEON)
        }
    }

    // Resource budget: limit number of functions to prevent DoS.
    if (program->functions.size() > kMaxFunctions) {
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error,
                                         {"", 0, 0},
                                         "Compilation aborted: function count limit exceeded (" +
                                             std::to_string(kMaxFunctions) + "). Input program is too large."});
    }

    // Validate: check for duplicate function names and duplicate parameters.
    bool hasMain = false;
    std::unordered_map<std::string, const FunctionDecl*> seenFunctions;
    for (auto& func : program->functions) {
        if (func->name == "main") {
            hasMain = true;
        }
        auto it = seenFunctions.find(func->name);
        if (it != seenFunctions.end()) {
            codegenError("Duplicate function definition: '" + func->name + "' (previously defined at line " +
                             std::to_string(it->second->line) + ")",
                         func.get());
        }
        seenFunctions[func->name] = func.get();

        // Check for duplicate parameter names within this function.
        std::unordered_set<std::string> seenParams;
        for (const auto& param : func->parameters) {
            if (!seenParams.insert(param.name).second) {
                codegenError("Duplicate parameter name '" + param.name + "' in function '" + func->name + "'",
                             func.get());
            }
        }

        if (func->isOptMax) {
            optMaxFunctions.insert(func->name);
        }
    }
    if (!hasMain) {
        // Program-level error — no specific AST node to reference for location.
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, "No 'main' function defined"});
    }

    // Set fast-math flags on the builder for all generated FP operations.
    if (useFastMath_) {
        llvm::FastMathFlags FMF;
        FMF.setFast(); // enables all unsafe FP optimizations
        builder->setFastMathFlags(FMF);
    } else if (optimizationLevel >= OptimizationLevel::O2) {
        // At O2+, enable FP contraction (a*b+c → fma) without full
        llvm::FastMathFlags FMF;
        FMF.setAllowContract(true);
        builder->setFastMathFlags(FMF);
    }

    // Forward-declare all functions so that any function can reference any
    for (auto& structDecl : program->structs) {
        for (auto& overload : structDecl->operators) {
            if (overload.impl) {
                const std::string key = structDecl->name + "::" + overload.op;
                operatorOverloads_[key] = overload.impl->name;
                program->functions.push_back(std::move(overload.impl));
            }
        }
    }

    for (auto& func : program->functions) {
        // Resolve parameter types from annotations: "float" → double, else i64.
        std::vector<llvm::Type*> paramTypes;
        paramTypes.reserve(func->parameters.size());
        for (auto& param : func->parameters) {
            // Type annotation enforcement: when a parameter has no explicit
            if (param.typeName.empty()) {
                paramTypes.push_back(getDefaultType());
            } else {
                paramTypes.push_back(resolveAnnotatedType(param.typeName));
            }
        }
        // Resolve return type from annotation (e.g. "-> float" → double).
        llvm::Type* retType = resolveAnnotatedType(func->returnType);
        llvm::FunctionType* funcType = llvm::FunctionType::get(retType, paramTypes, false);
        // Non-main functions use InternalLinkage at O2+ (equivalent to C's
        auto linkage = (func->name != "main" && optimizationLevel >= OptimizationLevel::O2)
                           ? llvm::Function::InternalLinkage
                           : llvm::Function::ExternalLinkage;
        llvm::Function* function =
            llvm::Function::Create(funcType, linkage, func->name, module.get());
        // Optimization-enabling attributes for all user functions:
        function->addFnAttr(llvm::Attribute::NoUnwind);
        function->addFnAttr(llvm::Attribute::MustProgress);
        // prefer-vector-width: use the target-aware preferred SIMD width
        function->addFnAttr("prefer-vector-width",
            std::to_string(preferredVectorWidth_ * 64));
        // nosync, nofree, willreturn — these promise the optimizer that the
        if (!usesConcurrencyPrimitive(func.get())) {
            function->addFnAttr(llvm::Attribute::NoSync);
            function->addFnAttr(llvm::Attribute::NoFree);
            function->addFnAttr(llvm::Attribute::WillReturn);
        }
        // noundef on all parameters and the return value — omscript always
        for (unsigned i = 0; i < func->parameters.size(); ++i) {
            function->addParamAttr(i, llvm::Attribute::NoUndef);
            if (paramTypes[i]->isIntegerTy()) {
                const auto& tn = func->parameters[i].typeName;
                if (isUnsignedAnnotation(tn))
                    function->addParamAttr(i, llvm::Attribute::ZExt);
                else
                    function->addParamAttr(i, llvm::Attribute::SExt);
            }
        }
        function->addRetAttr(llvm::Attribute::NoUndef);
        if (retType->isIntegerTy()) {
            if (isUnsignedAnnotation(func->returnType))
                function->addRetAttr(llvm::Attribute::ZExt);
            else
                function->addRetAttr(llvm::Attribute::SExt);
        }
        // For functions with pointer return types (-> string, -> int[], -> dict,
        if (retType->isPointerTy() && optimizationLevel >= OptimizationLevel::O1) {
            function->addRetAttr(llvm::Attribute::NonNull);
            function->addRetAttr(llvm::Attribute::getWithDereferenceableBytes(
                *context, 8));
        }
        functions[func->name] = function;
        functionDecls_[func->name] = func.get();
    }

    // Process enum declarations: store constant values for identifier resolution.
    for (auto& enumDecl : program->enums) {
        std::vector<std::string> memberNames;
        for (auto& [memberName, memberValue] : enumDecl->members) {
            const std::string fullName = enumDecl->name + "_" + memberName;
            enumConstants_[fullName] = memberValue;
            memberNames.push_back(memberName);
        }
        enumMembers_[enumDecl->name] = std::move(memberNames);
    }

    // Process struct declarations: store field layouts for struct operations.
    for (auto& structDecl : program->structs) {
        if (!structDecl->fieldDecls.empty()) {
            // Check if any field has hot or cold annotations.
            bool hasLayoutHints = false;
            for (const auto& fd : structDecl->fieldDecls) {
                if (fd.attrs.hot || fd.attrs.cold) {
                    hasLayoutHints = true;
                    break;
                }
            }

            if (hasLayoutHints && optimizationLevel >= OptimizationLevel::O2) {
                // Reorder: hot fields first, normal fields next, cold fields last.
                // Build a permutation that maps original index → new index.
                std::vector<size_t> hotIdx, normalIdx, coldIdx;
                for (size_t i = 0; i < structDecl->fieldDecls.size(); ++i) {
                    if (structDecl->fieldDecls[i].attrs.hot)
                        hotIdx.push_back(i);
                    else if (structDecl->fieldDecls[i].attrs.cold)
                        coldIdx.push_back(i);
                    else
                        normalIdx.push_back(i);
                }

                // Build reordered field lists.
                std::vector<std::string> reorderedFields;
                std::vector<StructField> reorderedDecls;
                reorderedFields.reserve(structDecl->fields.size());
                reorderedDecls.reserve(structDecl->fieldDecls.size());

                for (size_t i : hotIdx) {
                    reorderedFields.push_back(structDecl->fields[i]);
                    reorderedDecls.push_back(structDecl->fieldDecls[i]);
                }
                for (size_t i : normalIdx) {
                    reorderedFields.push_back(structDecl->fields[i]);
                    reorderedDecls.push_back(structDecl->fieldDecls[i]);
                }
                for (size_t i : coldIdx) {
                    reorderedFields.push_back(structDecl->fields[i]);
                    reorderedDecls.push_back(structDecl->fieldDecls[i]);
                }

                structDefs_[structDecl->name] = reorderedFields;
                structFieldDecls_[structDecl->name] = reorderedDecls;
            } else {
                structDefs_[structDecl->name] = structDecl->fields;
                structFieldDecls_[structDecl->name] = structDecl->fieldDecls;
            }
        } else {
            structDefs_[structDecl->name] = structDecl->fields;
        }
        // Eagerly build the LLVM StructType so that the field offsets/sizes
        getOrCreateStructLLVMType(structDecl->name);
    }

    // ── Optimization pre-pass sequence ────────────────────────────────────
    optCtx_ = std::make_unique<OptimizationContext>();
    optCtx_->setCTEngine(ctEngine_.get());
    optMgr_ = std::make_unique<OptimizationManager>();
    optMgr_->setCostModel(createDefaultCostModel());

    {
        OptimizationOrchestrator orch(optimizationLevel, verbose_, this, optMgr_.get());
        orch.runPrepasses(program, *optCtx_);
    }

    if (verbose_) {
        std::cout << "  [codegen] Generating LLVM IR for " << program->functions.size()
                  << " functions..." << '\n';
    }

    // Emit LLVM global variables for all program-level global declarations.
    generateGlobals(program);

    // Generate all function bodies
    for (auto& func : program->functions) {
        // Loop fusion pre-pass: merge adjacent @fuse-annotated loops
        if (func->body) {
            fuseLoops(func->body.get());
        }
        generateFunction(func.get());
    }

    // Infer memory effect attributes on user-defined functions.
    if (optimizationLevel >= OptimizationLevel::O1) {
        bool memEffChanged = true;
        while (memEffChanged) {
        memEffChanged = false;
        for (auto& func : module->functions()) {
            if (func.isDeclaration() || func.getName() == "main")
                continue;
            // Skip functions that already have explicit memory attributes.
            if (func.hasFnAttribute(llvm::Attribute::Memory))
                continue;
            // Scan all instructions: track whether the function reads/writes memory.
            bool hasMemoryWrite = false;
            bool hasMemoryRead = false;
            bool hasUnknownSideEffect = false;
            // Track whether all non-local memory accesses go through
            // function arguments.  If so, the function is argmem-only.
            bool allAccessesThroughArgs = true;
            // Helper: collect the set of function arguments for fast lookup.
            llvm::SmallPtrSet<llvm::Value*, 8> funcArgs;
            for (auto& arg : func.args())
                funcArgs.insert(&arg);
            // Helper: strip GEP chains to find the underlying allocation.
            auto stripPointerCasts = [](llvm::Value* ptr) -> llvm::Value* {
                for (unsigned i = 0; i < 16; ++i) { // depth limit
                    if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr)) {
                        ptr = gep->getPointerOperand();
                    } else if (auto* bc = llvm::dyn_cast<llvm::BitCastInst>(ptr)) {
                        ptr = bc->getOperand(0);
                    } else if (auto* i2p = llvm::dyn_cast<llvm::IntToPtrInst>(ptr)) {
                        ptr = i2p->getOperand(0);
                    } else if (auto* p2i = llvm::dyn_cast<llvm::PtrToIntInst>(ptr)) {
                        ptr = p2i->getOperand(0);
                    } else {
                        break;
                    }
                }
                return ptr;
            };
            auto isLocalAlloca = [&stripPointerCasts](llvm::Value* ptr) -> bool {
                return llvm::isa<llvm::AllocaInst>(stripPointerCasts(ptr));
            };
            // Helper: strip chains and check if the base is a function
            auto isArgDerived = [&](llvm::Value* ptr) -> bool {
                ptr = stripPointerCasts(ptr);
                if (funcArgs.count(ptr))
                    return true;
                // One level of load indirection: load from arg-derived ptr.
                if (auto* li = llvm::dyn_cast<llvm::LoadInst>(ptr)) {
                    llvm::Value* loadPtr = stripPointerCasts(li->getPointerOperand());
                    return funcArgs.count(loadPtr) > 0;
                }
                return false;
            };
            for (auto& BB : func) {
                for (auto& I : BB) {
                    if (auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                        // Stores to allocas (local variables) don't count as
                        if (!isLocalAlloca(SI->getPointerOperand())) {
                            hasMemoryWrite = true;
                            if (!isArgDerived(SI->getPointerOperand()))
                                allAccessesThroughArgs = false;
                        }
                    } else if (auto* LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
                        if (!isLocalAlloca(LI->getPointerOperand())) {
                            hasMemoryRead = true;
                            if (!isArgDerived(LI->getPointerOperand()))
                                allAccessesThroughArgs = false;
                        }
                    } else if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I)) {
                        auto* calledFn = CI->getCalledFunction();
                        if (!calledFn) {
                            hasUnknownSideEffect = true;
                            allAccessesThroughArgs = false;
                        } else {
                            // Check callee's memory effects.
                            auto ME = calledFn->getMemoryEffects();
                            if (!ME.onlyReadsMemory() && !ME.doesNotAccessMemory())
                                hasMemoryWrite = true;
                            if (!ME.doesNotAccessMemory())
                                hasMemoryRead = true;
                            if (ME == llvm::MemoryEffects::unknown()) {
                                hasUnknownSideEffect = true;
                                allAccessesThroughArgs = false;
                            }
                            // If callee accesses non-arg memory, this function
                            // transitively accesses non-arg memory too.
                            if (!ME.doesNotAccessMemory() &&
                                !calledFn->onlyAccessesArgMemory())
                                allAccessesThroughArgs = false;
                        }
                    }
                }
            }
            if (!hasUnknownSideEffect && !hasMemoryWrite && !hasMemoryRead) {
                // Function doesn't access memory at all (directly or
                // transitively through calls) → readnone.
                func.addFnAttr(llvm::Attribute::getWithMemoryEffects(
                    *context, llvm::MemoryEffects::none()));
                memEffChanged = true;
            } else if (!hasUnknownSideEffect && allAccessesThroughArgs) {
                // All non-local memory accesses go through function arguments.
                if (!hasMemoryWrite) {
                    func.addFnAttr(llvm::Attribute::getWithMemoryEffects(
                        *context, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
                } else {
                    func.addFnAttr(llvm::Attribute::getWithMemoryEffects(
                        *context, llvm::MemoryEffects::argMemOnly()));
                }
                memEffChanged = true;
            } else if (!hasUnknownSideEffect && !hasMemoryWrite) {
                // Function reads memory (possibly non-arg) → readonly.
                func.addFnAttr(llvm::Attribute::getWithMemoryEffects(
                    *context, llvm::MemoryEffects::readOnly()));
                memEffChanged = true;
            }
        } // end for each function
        } // end while memEffChanged

        // Speculatable inference: after memory effects have converged, any
        for (auto& func : module->functions()) {
            if (func.isDeclaration() || func.getName() == "main")
                continue;
            // Skip if already speculatable (set by @pure annotation).
            if (func.hasFnAttribute(llvm::Attribute::Speculatable))
                continue;
            // Requires memory(none) — no memory operations at all.
            if (!func.doesNotAccessMemory())
                continue;
            // Requires willreturn — the function always terminates.
            if (!func.hasFnAttribute(llvm::Attribute::WillReturn))
                continue;
            // Requires nounwind — already set for all user functions, but verify.
            if (!func.doesNotThrow())
                continue;
            // Exclude recursive functions to prevent infinite-recursion bugs.
            // A function is self-recursive if it directly calls itself.
            bool isSelfRecursive = false;
            for (auto& BB : func) {
                for (auto& I : BB) {
                    if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I)) {
                        if (CI->getCalledFunction() == &func) {
                            isSelfRecursive = true;
                            break;
                        }
                    }
                }
                if (isSelfRecursive) break;
            }
            if (isSelfRecursive) continue;
            func.addFnAttr(llvm::Attribute::Speculatable);
        }
    }

    // Infer norecurse attribute on user-defined functions.
    if (optimizationLevel >= OptimizationLevel::O1) {
        // Build a set of function names for quick lookup.
        std::unordered_set<std::string> allFunctions;
        for (auto& func : module->functions()) {
            if (!func.isDeclaration())
                allFunctions.insert(func.getName().str());
        }
        for (auto& func : module->functions()) {
            if (func.isDeclaration() || func.getName() == "main")
                continue;
            bool callsSelf = false;
            for (auto& BB : func) {
                for (auto& I : BB) {
                    if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I)) {
                        auto* calledFn = CI->getCalledFunction();
                        if (calledFn && calledFn->getName() == func.getName()) {
                            callsSelf = true;
                            break;
                        }
                        // Handle indirect calls through IntToPtr/PtrToInt chains:
                        if (!calledFn) {
                            llvm::Value* calledVal = CI->getCalledOperand();
                            for (unsigned d = 0; d < 8; ++d) {
                                if (auto* i2p = llvm::dyn_cast<llvm::IntToPtrInst>(calledVal))
                                    calledVal = i2p->getOperand(0);
                                else if (auto* p2i = llvm::dyn_cast<llvm::PtrToIntInst>(calledVal))
                                    calledVal = p2i->getOperand(0);
                                else if (auto* bc = llvm::dyn_cast<llvm::BitCastInst>(calledVal))
                                    calledVal = bc->getOperand(0);
                                else
                                    break;
                            }
                            if (auto* gv = llvm::dyn_cast<llvm::Function>(calledVal)) {
                                if (gv->getName() == func.getName()) {
                                    callsSelf = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (callsSelf) break;
            }
            if (!callsSelf) {
                func.addFnAttr(llvm::Attribute::NoRecurse);
            }
        }
    }

    // Apply OPTMAX per-function optimization passes BEFORE the module-wide IPO
    if (hasOptMaxFunctions && enableOptMax_) {
        if (verbose_) {
            std::cout << "  [opt] Running OPTMAX per-function optimization passes..." << '\n';
        }
        optimizeOptMaxFunctions();
    }

    // Mark all constant global variables with unnamed_addr at O2+.
    if (optimizationLevel >= OptimizationLevel::O2) {
        for (auto& gv : module->globals()) {
            if (!gv.isConstant()) continue;
            if (gv.getUnnamedAddr() != llvm::GlobalValue::UnnamedAddr::None) continue;
            // Don't annotate externally-visible globals with addresses that
            // external code might rely on (e.g. exported string constants).
            if (gv.hasExternalLinkage()) continue;
            // Skip globals that are address-taken in non-load instructions
            bool addrTakenAsValue = false;
            for (auto* user : gv.users()) {
                if (llvm::isa<llvm::LoadInst>(user)) continue;
                if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
                    // GEP is fine if all GEP users are loads.
                    bool gepAllLoads = true;
                    for (auto* gepUser : gep->users()) {
                        if (!llvm::isa<llvm::LoadInst>(gepUser)) {
                            gepAllLoads = false;
                            break;
                        }
                    }
                    if (gepAllLoads) continue;
                }
                if (auto* constExpr = llvm::dyn_cast<llvm::ConstantExpr>(user)) {
                    // GEP constant expr: also fine if only loaded from.
                    bool ceAllLoads = true;
                    for (auto* ceUser : constExpr->users()) {
                        if (!llvm::isa<llvm::LoadInst>(ceUser)) {
                            ceAllLoads = false;
                            break;
                        }
                    }
                    if (ceAllLoads) continue;
                }
                addrTakenAsValue = true;
                break;
            }
            if (!addrTakenAsValue) {
                gv.setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
            }
        }
    }

    if (verbose_) {
        std::cout << "  [opt] Running LLVM optimization pipeline..." << '\n';
    }
    if (runIRPasses_) {
        runOptimizationPasses();
    }

    // Finalize DWARF debug info before module verification.
    if (debugMode_ && debugBuilder_) {
        debugBuilder_->finalize();
    }

    // Verify the module
    std::string errorStr;
    llvm::raw_string_ostream errorStream(errorStr);
    if (llvm::verifyModule(*module, &errorStream)) {
        // Trim trailing whitespace from LLVM's verification output.
        while (!errorStr.empty() && (errorStr.back() == '\n' || errorStr.back() == ' ')) {
            errorStr.pop_back();
        }
        throw DiagnosticError(
            Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, "Module verification failed: " + errorStr});
    }

    // Print optimization statistics when verbose mode is enabled.
    if (verbose_) {
        // Pull in CF-CTRE / abstract-interpretation statistics so they appear
        if (ctEngine_) {
            const auto& s = ctEngine_->stats();
            optStats_.pureFunctions     = static_cast<unsigned>(s.pureFunctionsDetected);
            optStats_.uniformReturnFns  = static_cast<unsigned>(s.uniformReturnFunctionsFound);
            optStats_.deadFunctions     = static_cast<unsigned>(s.deadFunctionsDetected);
            optStats_.loopsReasoned     = static_cast<unsigned>(s.loopsReasoned);
            optStats_.branchMerges      = static_cast<unsigned>(s.branchMerges + s.ternaryMerges);
            optStats_.partialEvalFolds  = static_cast<unsigned>(s.partialEvalFolds);
            optStats_.algebraicFolds    = static_cast<unsigned>(s.algebraicFolds);
            optStats_.constraintFolds   = static_cast<unsigned>(s.constraintFolds);
            optStats_.deadBranches      = static_cast<unsigned>(s.deadBranchesEliminated);
            optStats_.safeArrayAccesses = static_cast<unsigned>(s.safeArrayAccesses);
            optStats_.safeDivisions     = static_cast<unsigned>(s.safeDivisions);
            optStats_.cheaperRewrites   = static_cast<unsigned>(s.cheaperRewritesFound);
        }
        optStats_.print();
    }
}

llvm::Function* CodeGenerator::generateFunction(FunctionDecl* func) {
    inOptMaxFunction = func->isOptMax;
    hasOptMaxFunctions = hasOptMaxFunctions || func->isOptMax;
    currentOptMaxConfig_ = func->optMaxConfig;
    if (func->isOptMax) {
        currentOptMaxConfig_.enabled = currentOptMaxConfig_.enabled || !func->optMaxConfig.enabled;
        // safety=off implies fastMath
        if (currentOptMaxConfig_.safety == SafetyLevel::Off) {
            currentOptMaxConfig_.fastMath = true;
        }
        // Store the resolved config for optimizeOptMaxFunctions() to consume later.
        // AST-level constant folding and algebraic simplification for @optmax
        // functions are handled by the pre-pass pipeline (CF-CTRE, AlgSimp,
        // CopyProp, DCE, CSE) which runs before code generation begins.
        optMaxFunctionConfigs_[func->name] = currentOptMaxConfig_;
    }

    // Retrieve the forward-declared function
    llvm::Function* function = functions[func->name];

    // --- Attach DWARF debug subprogram ---
    if (debugMode_ && debugBuilder_ && debugFile_) {
        llvm::DISubroutineType* debugFuncType =
            debugBuilder_->createSubroutineType(debugBuilder_->getOrCreateTypeArray({}));
        llvm::DISubprogram* SP = debugBuilder_->createFunction(
            debugFile_, func->name, llvm::StringRef(), debugFile_, func->line > 0 ? func->line : 1, debugFuncType,
            /*ScopeLine=*/func->line > 0 ? func->line : 1, llvm::DINode::FlagPrototyped,
            llvm::DISubprogram::SPFlagDefinition);
        function->setSubprogram(SP);
    }

    // Detect direct self-recursion.  Used for:
    bool isSelfRecursive = false;
    if (func->name != "main" && optimizationLevel >= OptimizationLevel::O1 && func->body) {
        // exprCheck: walk an expression tree for a direct call to func->name.
        std::function<bool(Expression*)> exprCheck = [&](Expression* e) -> bool {
            if (!e)
                return false;
            if (auto* call = dynamic_cast<CallExpr*>(e)) {
                if (call->callee == func->name)
                    return true;
                for (auto& arg : call->arguments)
                    if (exprCheck(arg.get()))
                        return true;
            }
            if (auto* bin = dynamic_cast<BinaryExpr*>(e))
                return exprCheck(bin->left.get()) || exprCheck(bin->right.get());
            if (auto* un = dynamic_cast<UnaryExpr*>(e))
                return exprCheck(un->operand.get());
            if (auto* tern = dynamic_cast<TernaryExpr*>(e))
                return exprCheck(tern->condition.get()) || exprCheck(tern->thenExpr.get()) ||
                       exprCheck(tern->elseExpr.get());
            return false;
        };
        // hasSelfCall: walk all statement kinds that can contain expressions.
        std::function<bool(Statement*)> hasSelfCall = [&](Statement* s) -> bool {
            if (!s)
                return false;
            if (auto* blk = dynamic_cast<BlockStmt*>(s)) {
                for (auto& st : blk->statements)
                    if (hasSelfCall(st.get()))
                        return true;
                return false;
            }
            if (auto* ret = dynamic_cast<ReturnStmt*>(s))
                return exprCheck(ret->value.get());
            if (auto* exprS = dynamic_cast<ExprStmt*>(s))
                return exprCheck(exprS->expression.get());
            if (auto* var = dynamic_cast<VarDecl*>(s))
                return exprCheck(var->initializer.get());
            if (auto* ifS = dynamic_cast<IfStmt*>(s))
                return exprCheck(ifS->condition.get()) || hasSelfCall(ifS->thenBranch.get()) ||
                       hasSelfCall(ifS->elseBranch.get());
            if (auto* wh = dynamic_cast<WhileStmt*>(s))
                return exprCheck(wh->condition.get()) || hasSelfCall(wh->body.get());
            if (auto* dw = dynamic_cast<DoWhileStmt*>(s))
                return hasSelfCall(dw->body.get()) || exprCheck(dw->condition.get());
            if (auto* fr = dynamic_cast<ForStmt*>(s))
                return exprCheck(fr->start.get()) || exprCheck(fr->end.get()) || exprCheck(fr->step.get()) ||
                       hasSelfCall(fr->body.get());
            if (auto* fe = dynamic_cast<ForEachStmt*>(s))
                return exprCheck(fe->collection.get()) || hasSelfCall(fe->body.get());
            return false;
        };
        isSelfRecursive = hasSelfCall(func->body.get());
        if (!isSelfRecursive) {
            function->addFnAttr(llvm::Attribute::NoRecurse);
        }
    }

    // Hint small helper functions for inlining at O2+.  OPTMAX functions
    static constexpr size_t kMaxInlineHintStatements = 10;
    static constexpr size_t kMaxInlineHintStatementsO3 = 20;
    static constexpr size_t kAlwaysInlineStatements = 8;
    // Recursive deep statement counter — counts all statements in nested
    std::function<size_t(Statement*)> deepStmtCount = [&](Statement* s) -> size_t {
        if (!s) return 0;
        if (auto* blk = dynamic_cast<BlockStmt*>(s)) {
            size_t cnt = 0;
            for (auto& st : blk->statements) cnt += deepStmtCount(st.get());
            return cnt;
        }
        if (auto* ifS = dynamic_cast<IfStmt*>(s))
            return 1 + deepStmtCount(ifS->thenBranch.get()) + deepStmtCount(ifS->elseBranch.get());
        if (auto* wh = dynamic_cast<WhileStmt*>(s))
            return 2 + 2 * deepStmtCount(wh->body.get());
        if (auto* dw = dynamic_cast<DoWhileStmt*>(s))
            return 2 + 2 * deepStmtCount(dw->body.get());
        if (auto* fr = dynamic_cast<ForStmt*>(s))
            return 2 + 2 * deepStmtCount(fr->body.get());
        if (auto* fe = dynamic_cast<ForEachStmt*>(s))
            return 2 + 2 * deepStmtCount(fe->body.get());
        return 1;
    };
    const size_t shallowCount = func->body ? func->body->statements.size() : 0;
    const size_t deepCount = func->body ? deepStmtCount(func->body.get()) : 0;
    const size_t inlineThreshold =
        (optimizationLevel >= OptimizationLevel::O3) ? kMaxInlineHintStatementsO3 : kMaxInlineHintStatements;
    // Zero-cost abstraction guarantee: lambda functions and very small
    static constexpr size_t kAlwaysInlineStatementsO2 = 4;
    const bool isLambda = func->name.rfind("__lambda_", 0) == 0;
    if (func->name != "main" && optimizationLevel >= OptimizationLevel::O1 && func->body &&
        shallowCount <= inlineThreshold) {
        if (optimizationLevel >= OptimizationLevel::O2) {
            const size_t alwaysInlineThreshold =
                (optimizationLevel >= OptimizationLevel::O3) ? kAlwaysInlineStatements : kAlwaysInlineStatementsO2;
            if ((isLambda || deepCount <= alwaysInlineThreshold) && !isSelfRecursive) {
                function->addFnAttr(llvm::Attribute::AlwaysInline);
            } else {
                function->addFnAttr(llvm::Attribute::InlineHint);
            }
        } else {
            // O1: hint small non-recursive functions for inlining only.
            if (!isSelfRecursive) {
                function->addFnAttr(llvm::Attribute::InlineHint);
            }
        }
    }

    // Apply user-specified function annotations.
    // These override the automatic inlining heuristics above.
    if (func->hintInline) {
        function->removeFnAttr(llvm::Attribute::NoInline);
        function->addFnAttr(llvm::Attribute::AlwaysInline);
    }
    if (func->hintNoInline) {
        function->removeFnAttr(llvm::Attribute::AlwaysInline);
        function->removeFnAttr(llvm::Attribute::InlineHint);
        function->addFnAttr(llvm::Attribute::NoInline);
    }
    if (func->hintCold) {
        function->addFnAttr(llvm::Attribute::Cold);
        userAnnotatedColdFunctions_.insert(func->name);
    }
    if (func->hintHot) {
        function->addFnAttr(llvm::Attribute::Hot);
        userAnnotatedHotFunctions_.insert(func->name);
        currentFuncHintHot_ = true;
    }
    if (func->hintPure) {
        // @pure: function has no side effects and does not read/write memory
        function->setOnlyReadsMemory();
        function->setDoesNotThrow();
        function->setDoesNotFreeMemory();
        // NoSync: the function does not communicate with other threads via
        function->setNoSync();
        // Speculatable + willreturn are only safe on non-recursive pure
        if (!isSelfRecursive) {
            function->setWillReturn();
            function->addFnAttr(llvm::Attribute::Speculatable);
        }
    }
    if (func->hintConstEval) {
        // @const_eval: mark the function for compile-time evaluation when
        if (optCtx_) optCtx_->mutableFacts(func->name).isConstFoldable = true;
        function->addFnAttr(llvm::Attribute::InlineHint);
        function->setOnlyReadsMemory();
        function->setDoesNotThrow();
        function->setWillReturn();
    }
    // Auto-apply LLVM memory-effect attributes based on inferred effects.
    if (!func->hintPure && optCtx_) {
        const FunctionEffects& fx = optCtx_->effects(func->name);
        if (fx.isReadNone()) {
            // No memory access, no I/O: readnone + nosync + willreturn
            function->setDoesNotAccessMemory();
            function->setNoSync();
            if (!isSelfRecursive) {
                function->setWillReturn();
                function->addFnAttr(llvm::Attribute::Speculatable);
            }
        } else if (fx.isReadOnly()) {
            // Reads memory but does not write or do I/O: readonly + nosync
            function->setOnlyReadsMemory();
            function->setNoSync();
            if (!isSelfRecursive)
                function->setWillReturn();
        } else if (fx.isNoSync()) {
            // Has writes but no I/O: nosync
            function->setNoSync();
        }

        // Overhauled effect-axis attribution (independent of memory effects):
        if (fx.isNoUnwind() && !func->hintNoUnwind /* already applied below */)
            function->setDoesNotThrow();
        if (fx.willReturn() && !isSelfRecursive)
            function->setWillReturn();
        if (!fx.isReadNone() && !fx.isReadOnly() && fx.argMemOnly()
            && !func->hintRestrict && !fileNoAlias_) {
            function->setOnlyAccessesArgMemory();
        }
        // Per-parameter readonly: tag pointer arguments that the analysis
        if (!fx.paramMutated.empty()) {
            const std::size_t n = std::min<std::size_t>(
                fx.paramMutated.size(), function->arg_size());
            for (std::size_t i = 0; i < n; ++i) {
                if (fx.paramMutated[i]) continue;
                auto* arg = function->getArg(static_cast<unsigned>(i));
                if (arg->getType()->isPointerTy())
                    arg->addAttr(llvm::Attribute::ReadOnly);
            }
        }
    }
    if (func->hintNoReturn) {
        function->addFnAttr(llvm::Attribute::NoReturn);
    }
    if (func->hintStatic) {
        // @static: use internal linkage so the function is only visible within
        function->setLinkage(llvm::GlobalValue::InternalLinkage);
    }
    if (func->hintFlatten) {
        // @flatten: inline all callees into this function.  This is the
        function->addFnAttr("flatten");
    }
    if (func->hintRestrict || fileNoAlias_) {
        // @restrict / @noalias / file-level @noalias: tell LLVM this function
        function->setOnlyAccessesArgMemory();
        function->setDoesNotThrow();
        // Mark all pointer parameters as noalias — OmScript's ownership
        for (unsigned i = 0; i < function->arg_size(); ++i) {
            if (function->getArg(i)->getType()->isPointerTy()) {
                function->addParamAttr(i, llvm::Attribute::NoAlias);
            }
        }
    }

    if (func->hintMinSize) {
        // @minsize: optimize for minimum code size.  Maps to LLVM's OptimizeForSize
        function->addFnAttr(llvm::Attribute::OptimizeForSize);
        function->addFnAttr(llvm::Attribute::MinSize);
    }
    if (func->hintOptNone) {
        // @optnone: disable all optimizations for this function.  Maps to LLVM's
        function->addFnAttr(llvm::Attribute::OptimizeNone);
        // OptimizeNone requires NoInline per LLVM verifier rules.
        function->addFnAttr(llvm::Attribute::NoInline);
        function->removeFnAttr(llvm::Attribute::AlwaysInline);
        function->removeFnAttr(llvm::Attribute::InlineHint);
    }
    if (func->hintNoUnwind) {
        // @nounwind: function never throws C++ exceptions.  Maps to LLVM's
        function->addFnAttr(llvm::Attribute::NoUnwind);
    }

    // @allocator(size=N) / @allocator(size=N, count=M): mark as allocator wrapper.
    if (func->allocatorSizeParam >= 0) {
        const unsigned sizeIdx  = static_cast<unsigned>(func->allocatorSizeParam);
        if (func->allocatorCountParam >= 0) {
            const unsigned countIdx = static_cast<unsigned>(func->allocatorCountParam);
            function->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(*context, sizeIdx, countIdx));
        } else {
            function->addFnAttr(llvm::Attribute::getWithAllocSizeArgs(*context, sizeIdx, std::nullopt));
        }
        // noalias is only valid on pointer return types.
        if (function->getReturnType()->isPointerTy()) {
            function->addRetAttr(llvm::Attribute::NoAlias);
        }
        function->addFnAttr(llvm::Attribute::WillReturn);
        function->addFnAttr(llvm::Attribute::NoUnwind);
        optStats_.allocatorFuncs++;
    }

    // OmScript uses a flag-based error model (not C++ exceptions / DWARF
    if (optimizationLevel >= OptimizationLevel::O2 &&
        !function->hasFnAttribute(llvm::Attribute::NoUnwind)) {
        function->addFnAttr(llvm::Attribute::NoUnwind);
    }

    // At O2+, align function entry to 16 bytes for better I-cache locality
    if (optimizationLevel >= OptimizationLevel::O2) {
        function->setAlignment(func->hintHot ? llvm::Align(32) : llvm::Align(16));

        // mustprogress: tells LLVM that every loop in this function will
        if (!function->hasFnAttribute(llvm::Attribute::OptimizeNone)) {
            function->addFnAttr(llvm::Attribute::MustProgress);
        }

        // OmScript's ownership model guarantees that pointer parameters
        for (unsigned i = 0; i < function->arg_size(); ++i) {
            if (function->getArg(i)->getType()->isPointerTy()) {
                function->addParamAttr(i, llvm::Attribute::NonNull);
            }
        }
    }

    // In OPTMAX functions, mark all parameters noalias and add WillReturn.
    if (inOptMaxFunction) {
        // OPTMAX is the user's guarantee that this function always terminates,
        function->addFnAttr(llvm::Attribute::WillReturn);
        function->addFnAttr(llvm::Attribute::NoSync);
        for (unsigned i = 0; i < function->arg_size(); ++i) {
            if (function->getArg(i)->getType()->isPointerTy()) {
                function->addParamAttr(i, llvm::Attribute::NoAlias);
                function->addParamAttr(i, llvm::Attribute::NonNull);
                // OmScript arrays always have a valid header (at least 8 bytes
                function->addParamAttr(i, llvm::Attribute::getWithDereferenceableBytes(
                    *context, 8));
                // OmScript arrays/strings are allocated via calloc/malloc which
                function->addParamAttr(i, llvm::Attribute::getWithAlignment(
                    *context, llvm::Align(16)));
                OMSC_ADD_NOCAPTURE(function, i);
            }
        }
    }

    // @hot functions at O2+: add nonnull and noalias on pointer parameters.
    if (currentFuncHintHot_ && optimizationLevel >= OptimizationLevel::O2 && !inOptMaxFunction) {
        for (unsigned i = 0; i < function->arg_size(); ++i) {
            if (function->getArg(i)->getType()->isPointerTy()) {
                function->addParamAttr(i, llvm::Attribute::NonNull);
                function->addParamAttr(i, llvm::Attribute::NoAlias);
                // OmScript arrays always have a valid header (at least 8 bytes).
                function->addParamAttr(i, llvm::Attribute::getWithDereferenceableBytes(
                    *context, 8));
                function->addParamAttr(i, llvm::Attribute::getWithAlignment(
                    *context, llvm::Align(16)));
                OMSC_ADD_NOCAPTURE(function, i);
            }
        }
    }

    // Default noalias fallback: OmScript's ownership model guarantees that
    if (!inOptMaxFunction && !currentFuncHintHot_
        && !func->hintRestrict && !fileNoAlias_) {
        for (unsigned i = 0; i < function->arg_size(); ++i) {
            if (function->getArg(i)->getType()->isPointerTy()) {
                function->addParamAttr(i, llvm::Attribute::NoAlias);
                function->addParamAttr(i, llvm::Attribute::NonNull);
                // OmScript arrays always have a valid header: at least 8 bytes
                function->addParamAttr(i, llvm::Attribute::getWithDereferenceableBytes(
                    *context, 8));
                // OmScript arrays/strings are allocated by calloc/malloc which
                function->addParamAttr(i, llvm::Attribute::getWithAlignment(
                    *context, llvm::Align(16)));
                // OmScript's ownership model ensures pointer parameters are
                OMSC_ADD_NOCAPTURE(function, i);
            }
        }
    }

    // OmScript is a single-threaded language — no concurrent memory access
    if (optimizationLevel >= OptimizationLevel::O2 && !inOptMaxFunction) {
        function->addFnAttr(llvm::Attribute::NoSync);
    }

    // Emit range return attribute when a narrowed ValueRange is known for
#if LLVM_VERSION_MAJOR >= 19
    if (optCtx_ && function->getReturnType()->isIntegerTy(64)) {
        if (auto rng = optCtx_->returnRange(func->name)) {
            if (rng->isNarrowed() && !rng->isEmpty() &&
                rng->hi < std::numeric_limits<int64_t>::max()) {
                llvm::APInt apLo(64, static_cast<uint64_t>(rng->lo), /*isSigned=*/true);
                llvm::APInt apHi(64, static_cast<uint64_t>(rng->hi + 1), /*isSigned=*/true);
                function->addRetAttr(llvm::Attribute::getWithRange(
                    *context, llvm::ConstantRange(apLo, apHi)));
            }
        }
    }
#endif

    // @unroll / @nounroll: per-function loop unrolling control.
    // These are stored and applied to every loop emitted within this function.
    currentFuncHintUnroll_ = func->hintUnroll;
    currentFuncHintNoUnroll_ = func->hintNoUnroll;
    currentFuncDecl_ = func;

    // @vectorize / @novectorize: per-function loop vectorization control.
    // These are stored and applied to every loop emitted within this function.
    currentFuncHintVectorize_ = func->hintVectorize;
    currentFuncHintNoVectorize_ = func->hintNoVectorize;

    // @parallel / @noparallel: per-function auto-parallelization control.
    // These are stored and applied to every loop emitted within this function.
    currentFuncHintParallelize_ = func->hintParallelize;
    currentFuncHintNoParallelize_ = func->hintNoParallelize;

    // @hot: per-function hot annotation.  Used for bounds check elimination
    // and other performance-critical optimizations.
    currentFuncHintHot_ = func->hintHot;

    // @optmax: enable full fast-math flags for all float operations in this
    const llvm::FastMathFlags savedFMF = builder->getFastMathFlags();
    if (inOptMaxFunction && !useFastMath_ && currentOptMaxConfig_.fastMath) {
        // fast_math=true (or safety=off, which implies it): enable all fast-math
        llvm::FastMathFlags FMF;
        FMF.setFast();
        builder->setFastMathFlags(FMF);
        // Also set function-level string attributes so LTO and the back-end
        // can see the fast-math contract without inspecting every instruction.
        function->addFnAttr("unsafe-fp-math", "true");
        function->addFnAttr("no-nans-fp-math", "true");
        function->addFnAttr("no-infs-fp-math", "true");
        function->addFnAttr("no-signed-zeros-fp-math", "true");
        function->addFnAttr("approx-func-fp-math", "true");
        function->addFnAttr("no-trapping-math", "true");
    }

    // Create entry basic block
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context, "entry", function);
    builder->SetInsertPoint(entry);

    // Set parameter names and create allocas
    namedValues.clear();
    scopeStack.clear();
    loopStack.clear();
    constValues.clear();
    constScopeStack.clear();
    stringVars_.clear();
    stringArrayVars_.clear();
    stringLenCache_.clear();
    stringCapCache_.clear();
    deadVars_.clear();
    deadVarReason_.clear();
    varBorrowStates_.clear();
    borrowMap_.clear();
    borrowScopeStack_.clear();
    frozenVars_.clear();
    prefetchedParams_.clear();
    prefetchedVars_.clear();
    prefetchedImmutVars_.clear();
    registerVars_.clear();
    simdVars_.clear();
    dictVarNames_.clear();
    nonNegValues_.clear();
    constIntFolds_.clear();
    constFloatFolds_.clear();
    stackAllocatedArrays_.clear();
    readOnlyGlobalArrays_.clear();
    pendingArrayStackAlloc_ = false;
    pendingArrayReadOnlyGlobal_ = false;
    scopeComptimeInts_.clear();
    catchTable_.clear();
    catchDefaultBB_ = nullptr;

    // Expose all global variables inside this function so that reads,
    // writes, and assignments resolve through the normal namedValues path.
    for (auto& entry : globalVars_) {
        namedValues[entry.getKey().str()] = entry.getValue();
    }

    // Pre-populate stringVars_ for parameters known to receive string arguments.
    auto paramStrIt = funcParamStringTypes_.find(func->name);
    auto paramArrIt = funcParamArrayTypes_.find(func->name);
    auto argIt = function->arg_begin();
    for (size_t paramIdx = 0; paramIdx < func->parameters.size(); ++paramIdx) {
        auto& param = func->parameters[paramIdx];
        argIt->setName(param.name);

        // Use the parameter's actual LLVM type (respects type annotations).
        llvm::AllocaInst* alloca = createEntryBlockAlloca(function, param.name, argIt->getType());
        builder->CreateStore(&(*argIt), alloca);
        bindVariable(param.name, alloca);
        // Annotate parameter with its declared type for signed/unsigned tracking.
        if (!param.typeName.empty())
            varTypeAnnotations_[param.name] = param.typeName;

        if (paramStrIt != funcParamStringTypes_.end() && paramStrIt->second.count(paramIdx))
            stringVars_.insert(param.name);
        // Pre-populate arrayVars_ for parameters known to receive array arguments.
        if (paramArrIt != funcParamArrayTypes_.end() && paramArrIt->second.count(paramIdx))
            arrayVars_.insert(param.name);

        // @prefetch: emit llvm.prefetch at function entry for annotated params.
        if (param.hintPrefetch) {
            prefetchedParams_.insert(param.name);
            llvm::Value* ptrVal = builder->CreateLoad(argIt->getType(), alloca, param.name + ".pf.load");
            // Cast to ptr for the prefetch intrinsic (parameter value is treated as a pointer)
            llvm::Value* ptr = builder->CreateIntToPtr(ptrVal, llvm::PointerType::getUnqual(*context), param.name + ".pf.ptr");
            llvm::Function* prefetchFn = OMSC_GET_INTRINSIC(
                module.get(), llvm::Intrinsic::prefetch,
                {llvm::PointerType::getUnqual(*context)});
            // Args: ptr, rw (0=read), locality (3=keep in all cache levels), cache_type (1=data)
            builder->CreateCall(prefetchFn, {
                ptr,
                builder->getInt32(0),  // read prefetch
                builder->getInt32(3),  // high temporal locality
                builder->getInt32(1)   // data cache
            });
        }

        ++argIt;
    }

    // @optmax assumes=[...]: emit llvm.assume intrinsics for each assertion
    if (inOptMaxFunction && !currentOptMaxConfig_.assumes.empty()) {
        llvm::Function* assumeIntr = OMSC_GET_INTRINSIC(
            module.get(), llvm::Intrinsic::assume);
        for (const auto& assumeStr : currentOptMaxConfig_.assumes) {
            // Tokenise: split on whitespace into [var, op, literal]
            std::istringstream ss(assumeStr);
            std::string varName, op, litStr;
            if (!(ss >> varName >> op >> litStr)) continue;
            int64_t litVal = 0;
            try { litVal = std::stoll(litStr); } catch (...) { continue; }
            // Look up the variable in namedValues (covers all parameters just stored).
            auto nv = namedValues.find(varName);
            if (nv == namedValues.end()) continue;
            auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(nv->second);
            if (!alloca) continue;
            llvm::Value* loaded = builder->CreateLoad(
                alloca->getAllocatedType(), alloca, varName + ".assume.load");
            // Cast to i64 for comparison (handles both int and float params).
            llvm::Value* cmpVal = loaded;
            if (cmpVal->getType()->isDoubleTy())
                cmpVal = builder->CreateFPToSI(cmpVal, getDefaultType(), varName + ".assume.fptoi");
            else if (cmpVal->getType() != getDefaultType())
                cmpVal = builder->CreateSExtOrBitCast(cmpVal, getDefaultType(), varName + ".assume.cast");
            llvm::Value* lit = llvm::ConstantInt::get(getDefaultType(), litVal, /*isSigned=*/true);
            llvm::Value* cond = nullptr;
            if      (op == ">")  cond = builder->CreateICmpSGT(cmpVal, lit, varName + ".assume.cond");
            else if (op == ">=") cond = builder->CreateICmpSGE(cmpVal, lit, varName + ".assume.cond");
            else if (op == "<")  cond = builder->CreateICmpSLT(cmpVal, lit, varName + ".assume.cond");
            else if (op == "<=") cond = builder->CreateICmpSLE(cmpVal, lit, varName + ".assume.cond");
            else if (op == "!=") cond = builder->CreateICmpNE (cmpVal, lit, varName + ".assume.cond");
            else if (op == "==") cond = builder->CreateICmpEQ (cmpVal, lit, varName + ".assume.cond");
            if (cond) {
                builder->CreateCall(assumeIntr, {cond});
                nonNegValues_.insert(cmpVal);  // help downstream range analysis
            }
        }
    }

    // @optmax memory.prefetch=true: auto-prefetch all pointer-type parameters
    if (inOptMaxFunction && currentOptMaxConfig_.memory.prefetch) {
        llvm::Function* prefetchFn = OMSC_GET_INTRINSIC(
            module.get(), llvm::Intrinsic::prefetch,
            {llvm::PointerType::getUnqual(*context)});
        for (size_t i = 0; i < func->parameters.size(); ++i) {
            const auto& param = func->parameters[i];
            if (prefetchedParams_.count(param.name)) continue;  // already prefetched
            auto it = namedValues.find(param.name);
            if (it == namedValues.end()) continue;
            auto* al = llvm::dyn_cast<llvm::AllocaInst>(it->second);
            if (!al) continue;
            llvm::Value* val = builder->CreateLoad(al->getAllocatedType(), al,
                                                   param.name + ".autopf.load");
            // Treat as a pointer (i64 → ptr cast) and prefetch the target memory.
            llvm::Value* ptr = val->getType()->isPointerTy()
                ? val
                : builder->CreateIntToPtr(val, llvm::PointerType::getUnqual(*context),
                                          param.name + ".autopf.ptr");
            builder->CreateCall(prefetchFn, {
                ptr,
                builder->getInt32(0),  // read prefetch
                builder->getInt32(3),  // high temporal locality — keep in all levels
                builder->getInt32(1)   // data cache
            });
            prefetchedParams_.insert(param.name);
        }
    }

    // Pre-pass: collect all catch(code) blocks in this function body,
    buildCatchTable(func->body->statements, function);

    // Generate function body
    generateBlock(func->body.get());

    // Add default return if needed
    if (!builder->GetInsertBlock()->getTerminator()) {
        // @prefetch enforcement: at function exit, emit cache invalidation
        for (const auto& pfParam : prefetchedParams_) {
            auto it = namedValues.find(pfParam);
            if (it != namedValues.end()) {
                llvm::Value* ptrVal = builder->CreateLoad(
                    llvm::cast<llvm::AllocaInst>(it->second)->getAllocatedType(),
                    it->second, pfParam + ".pf.exit");
                llvm::Value* ptr = builder->CreateIntToPtr(
                    ptrVal, llvm::PointerType::getUnqual(*context), pfParam + ".pf.evict");
                llvm::Function* prefetchFn = OMSC_GET_INTRINSIC(
                    module.get(), llvm::Intrinsic::prefetch,
                    {llvm::PointerType::getUnqual(*context)});
                // locality=0 means "no temporal reuse" — hint CPU to evict
                builder->CreateCall(prefetchFn, {
                    ptr,
                    builder->getInt32(0),  // read
                    builder->getInt32(0),  // locality=0: evict from cache
                    builder->getInt32(1)   // data cache
                });
            }
        }
        llvm::Type* retTy = function->getReturnType();
        if (retTy->isDoubleTy())
            builder->CreateRet(llvm::ConstantFP::get(retTy, 0.0));
        else
            builder->CreateRet(llvm::ConstantInt::get(*context, llvm::APInt(64, 0)));
    }

    // Verify function
    std::string errorStr;
    llvm::raw_string_ostream errorStream(errorStr);
    if (llvm::verifyFunction(*function, &errorStream)) {
        function->print(llvm::errs());
        // Trim trailing whitespace from LLVM's verification output.
        while (!errorStr.empty() && (errorStr.back() == '\n' || errorStr.back() == ' ')) {
            errorStr.pop_back();
        }
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error,
                                         {"", func->line, func->column},
                                         "Function verification failed for '" + func->name + "': " + errorStr});
    }

    // Force register promotion for functions that use the `register` keyword.
    if (!registerVars_.empty()) {
        llvm::legacy::FunctionPassManager fpm(module.get());
        fpm.add(llvm::createPromoteMemoryToRegisterPass());
        fpm.doInitialization();
        fpm.run(*function);
        fpm.doFinalization();
    }

    inOptMaxFunction = false;

    // Restore fast-math flags to pre-OPTMAX state so subsequent functions
    // are not affected by the aggressive flags set for this function.
    builder->setFastMathFlags(savedFMF);

    return function;
}

void CodeGenerator::generateStatement(Statement* stmt) {
    checkIRBudget();
    switch (stmt->type) {
    case ASTNodeType::VAR_DECL:
        generateVarDecl(static_cast<VarDecl*>(stmt));
        break;
    case ASTNodeType::RETURN_STMT:
        generateReturn(static_cast<ReturnStmt*>(stmt));
        break;
    case ASTNodeType::IF_STMT:
        generateIf(static_cast<IfStmt*>(stmt));
        break;
    case ASTNodeType::WHILE_STMT:
        generateWhile(static_cast<WhileStmt*>(stmt));
        break;
    case ASTNodeType::DO_WHILE_STMT:
        generateDoWhile(static_cast<DoWhileStmt*>(stmt));
        break;
    case ASTNodeType::FOR_STMT:
        generateFor(static_cast<ForStmt*>(stmt));
        break;
    case ASTNodeType::FOR_EACH_STMT:
        generateForEach(static_cast<ForEachStmt*>(stmt));
        break;
    case ASTNodeType::BREAK_STMT:
        if (loopStack.empty()) {
            codegenError("break used outside of a loop", stmt);
        }
        builder->CreateBr(loopStack.back().breakTarget);
        break;
    case ASTNodeType::CONTINUE_STMT: {
        // Search backwards through the loop stack for the nearest enclosing loop
        llvm::BasicBlock* continueTarget = nullptr;
        for (auto it = loopStack.rbegin(); it != loopStack.rend(); ++it) {
            if (it->continueTarget != nullptr) {
                continueTarget = it->continueTarget;
                break;
            }
        }
        if (!continueTarget) {
            codegenError("continue used outside of a loop", stmt);
        }
        builder->CreateBr(continueTarget);
        break;
    }
    case ASTNodeType::BLOCK:
        generateBlock(static_cast<BlockStmt*>(stmt));
        break;
    case ASTNodeType::EXPR_STMT:
        generateExprStmt(static_cast<ExprStmt*>(stmt));
        break;
    case ASTNodeType::SWITCH_STMT:
        generateSwitch(static_cast<SwitchStmt*>(stmt));
        break;
    case ASTNodeType::CATCH_STMT:
        generateCatch(static_cast<CatchStmt*>(stmt));
        break;
    case ASTNodeType::THROW_STMT:
        generateThrow(static_cast<ThrowStmt*>(stmt));
        break;
    case ASTNodeType::ENUM_DECL:
    case ASTNodeType::STRUCT_DECL:
        // Enums and structs are handled at program level, nothing to do here.
        break;
    case ASTNodeType::INVALIDATE_STMT:
        generateInvalidate(static_cast<InvalidateStmt*>(stmt));
        break;
    case ASTNodeType::MOVE_DECL:
        generateMoveDecl(static_cast<MoveDecl*>(stmt));
        break;
    case ASTNodeType::FREEZE_STMT:
        generateFreeze(static_cast<FreezeStmt*>(stmt));
        break;
    case ASTNodeType::PREFETCH_STMT:
        generatePrefetch(static_cast<PrefetchStmt*>(stmt));
        break;
    case ASTNodeType::ASSUME_STMT:
        generateAssume(static_cast<AssumeStmt*>(stmt));
        break;
    case ASTNodeType::DEFER_STMT:
        // Defer outside a block: just execute the body immediately
        // (normal defer semantics are handled in generateBlock)
        generateStatement(static_cast<DeferStmt*>(stmt)->body.get());
        break;
    case ASTNodeType::PIPELINE_STMT:
        generatePipeline(static_cast<PipelineStmt*>(stmt));
        break;
    default:
        codegenError("Unknown statement type", stmt);
    }
}

llvm::Value* CodeGenerator::generateExpression(Expression* expr) {
    checkIRBudget();
    switch (expr->type) {
    case ASTNodeType::LITERAL_EXPR:
        return generateLiteral(static_cast<LiteralExpr*>(expr));
    case ASTNodeType::IDENTIFIER_EXPR:
        return generateIdentifier(static_cast<IdentifierExpr*>(expr));
    case ASTNodeType::BINARY_EXPR:
        return generateBinary(static_cast<BinaryExpr*>(expr));
    case ASTNodeType::UNARY_EXPR:
        return generateUnary(static_cast<UnaryExpr*>(expr));
    case ASTNodeType::CALL_EXPR:
        return generateCall(static_cast<CallExpr*>(expr));
    case ASTNodeType::ASSIGN_EXPR:
        return generateAssign(static_cast<AssignExpr*>(expr));
    case ASTNodeType::POSTFIX_EXPR:
        return generatePostfix(static_cast<PostfixExpr*>(expr));
    case ASTNodeType::PREFIX_EXPR:
        return generatePrefix(static_cast<PrefixExpr*>(expr));
    case ASTNodeType::TERNARY_EXPR:
        return generateTernary(static_cast<TernaryExpr*>(expr));
    case ASTNodeType::ARRAY_EXPR:
        return generateArray(static_cast<ArrayExpr*>(expr));
    case ASTNodeType::INDEX_EXPR:
        return generateIndex(static_cast<IndexExpr*>(expr));
    case ASTNodeType::INDEX_ASSIGN_EXPR:
        return generateIndexAssign(static_cast<IndexAssignExpr*>(expr));
    case ASTNodeType::STRUCT_LITERAL_EXPR:
        return generateStructLiteral(static_cast<StructLiteralExpr*>(expr));
    case ASTNodeType::FIELD_ACCESS_EXPR:
        return generateFieldAccess(static_cast<FieldAccessExpr*>(expr));
    case ASTNodeType::FIELD_ASSIGN_EXPR:
        return generateFieldAssign(static_cast<FieldAssignExpr*>(expr));
    case ASTNodeType::PIPE_EXPR: {
        // Desugar: expr |> fn  =>  fn(expr)
        auto* pipe = static_cast<PipeExpr*>(expr);
        // Create a synthetic CallExpr and generate it
        std::vector<std::unique_ptr<Expression>> args;
        args.push_back(std::move(pipe->left));
        auto callExpr = std::make_unique<CallExpr>(pipe->functionName, std::move(args));
        callExpr->fromStdNamespace = true; // pipe-forward desugaring
        callExpr->line = pipe->line;
        callExpr->column = pipe->column;
        return generateCall(callExpr.get());
    }
    case ASTNodeType::LAMBDA_EXPR:
        // Lambdas are desugared by the parser to string literals
        codegenError("Lambda expression should have been desugared by parser", expr);
    case ASTNodeType::SPREAD_EXPR:
        // Spread expressions are only valid inside array literals
        codegenError("Spread operator '...' is only valid inside array literals", expr);
    case ASTNodeType::MOVE_EXPR:
        return generateMoveExpr(static_cast<MoveExpr*>(expr));
    case ASTNodeType::BORROW_EXPR:
        return generateBorrowExpr(static_cast<BorrowExpr*>(expr));
    case ASTNodeType::REBORROW_EXPR:
        return generateReborrowExpr(static_cast<ReborrowExpr*>(expr));
    case ASTNodeType::DICT_EXPR:
        return generateDict(static_cast<DictExpr*>(expr));
    case ASTNodeType::SCOPE_RESOLUTION_EXPR:
        return generateScopeResolution(static_cast<ScopeResolutionExpr*>(expr));
    case ASTNodeType::RANGE_ANNOT_EXPR:
        return generateRangeAnnot(static_cast<RangeAnnotExpr*>(expr));
    case ASTNodeType::COMPTIME_EXPR: {
        // comptime { ... } — evaluate the block at compile time via CF-CTRE.
        auto* ct = static_cast<ComptimeExpr*>(expr);
        if (!ctEngine_)
            codegenError("comptime block requires CF-CTRE (internal error)", expr);
        auto ctResult = ctEngine_->evalComptimeBlock(ct->body.get(), buildComptimeEnv());
        if (!ctResult)
            codegenError("comptime block could not be evaluated at compile time", expr);
        // Stash so the VarDecl handler can register it in the fold maps.
        lastComptimeCtResult_ = *ctResult;
        if (ctResult->isInt())
            return llvm::ConstantInt::get(getDefaultType(), ctResult->asI64(), /*isSigned=*/true);
        if (ctResult->isString()) {
            llvm::GlobalVariable* gv = internString(ctResult->asStr());
            return llvm::ConstantExpr::getInBoundsGetElementPtr(
                gv->getValueType(), gv,
                llvm::ArrayRef<llvm::Constant*>{
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)});
        }
        if (ctResult->isArray()) {
            auto cv = ctValueToConstValue(*ctResult);
            return emitComptimeArray(cv.arrVal);
        }
        codegenError("comptime block returned an unsupported type (only int/string/array supported)", expr);
    }
    default:
        codegenError("Unknown expression type", expr);
    }
}

} // namespace omscript

// ---------------------------------------------------------------------------
namespace omscript {

std::optional<int64_t> CodeGenerator::tryConstEval(
    const FunctionDecl* func,
    const std::vector<int64_t>& argVals) {

    if (!func || !func->body || func->parameters.size() != argVals.size())
        return std::nullopt;

    // Environment: variable name → current integer value
    std::unordered_map<std::string, int64_t> env;
    for (size_t i = 0; i < argVals.size(); ++i) {
        env[func->parameters[i].name] = argVals[i];
    }

    // Limit recursion depth to prevent infinite loops at compile time
    static thread_local int depth = 0;
    if (++depth > 100) { --depth; return std::nullopt; }

    struct DepthGuard { ~DepthGuard() { --depth; } } const guard;

    // Return value (set by a ReturnStmt)
    std::optional<int64_t> retVal;

    // Forward declarations for mutual recursion
    std::function<std::optional<int64_t>(Expression*)> evalExpr;
    std::function<bool(Statement*)> evalStmt;

    evalExpr = [&](Expression* e) -> std::optional<int64_t> {
        if (!e) return std::nullopt;
        switch (e->type) {
        case ASTNodeType::LITERAL_EXPR: {
            auto* lit = static_cast<LiteralExpr*>(e);
            if (lit->literalType == LiteralExpr::LiteralType::INTEGER)
                return lit->intValue;
            // Booleans are represented as integers in OmScript (0 or 1).
            // Float literals are not supported in const_eval context.
            return std::nullopt;
        }
        case ASTNodeType::IDENTIFIER_EXPR: {
            auto* id = static_cast<IdentifierExpr*>(e);
            auto it = env.find(id->name);
            if (it != env.end()) return it->second;
            // Check enum constants
            auto eit = enumConstants_.find(id->name);
            if (eit != enumConstants_.end()) return static_cast<int64_t>(eit->second);
            return std::nullopt;
        }
        case ASTNodeType::BINARY_EXPR: {
            auto* bin = static_cast<BinaryExpr*>(e);
            auto lv = evalExpr(bin->left.get());
            // Short-circuit for logical operators
            if (bin->op == "&&") {
                if (!lv) return std::nullopt;
                if (*lv == 0) return int64_t(0);
                auto rv = evalExpr(bin->right.get());
                return rv ? std::optional<int64_t>(*rv != 0 ? 1 : 0) : std::nullopt;
            }
            if (bin->op == "||") {
                if (!lv) return std::nullopt;
                if (*lv != 0) return int64_t(1);
                auto rv = evalExpr(bin->right.get());
                return rv ? std::optional<int64_t>(*rv != 0 ? 1 : 0) : std::nullopt;
            }
            auto rv = evalExpr(bin->right.get());
            if (!lv || !rv) return std::nullopt;
            const int64_t a = *lv, b = *rv;
            // Wrapping arithmetic helper: cast to uint64_t to avoid signed
            // overflow UB, then cast the bit-pattern back to int64_t.
            auto wrap = [](int64_t x, int64_t y, char op) -> int64_t {
                const auto ux = static_cast<uint64_t>(x);
                const auto uy = static_cast<uint64_t>(y);
                switch (op) {
                case '+': return static_cast<int64_t>(ux + uy);
                case '-': return static_cast<int64_t>(ux - uy);
                case '*': return static_cast<int64_t>(ux * uy);
                default:  return 0;
                }
            };
            // Use wrapping arithmetic for +, -, * to match i64 runtime semantics.
            if (bin->op == "+") return wrap(a, b, '+');
            if (bin->op == "-") return wrap(a, b, '-');
            if (bin->op == "*") return wrap(a, b, '*');
            // Guard against /0 and the INT64_MIN/-1 trap.
            if (bin->op == "/" && b != 0) {
                if (a == std::numeric_limits<int64_t>::min() && b == -1) return std::nullopt;
                return a / b;
            }
            if (bin->op == "%" && b != 0) {
                if (a == std::numeric_limits<int64_t>::min() && b == -1) return int64_t(0);
                return a % b;
            }
            if (bin->op == "&") return a & b;
            if (bin->op == "|") return a | b;
            if (bin->op == "^") return a ^ b;
            if (bin->op == "<<" && b >= 0 && b < 64) return a << b;
            if (bin->op == ">>" && b >= 0 && b < 64) return a >> b;
            if (bin->op == "==") return int64_t(a == b ? 1 : 0);
            if (bin->op == "!=") return int64_t(a != b ? 1 : 0);
            if (bin->op == "<")  return int64_t(a < b ? 1 : 0);
            if (bin->op == "<=") return int64_t(a <= b ? 1 : 0);
            if (bin->op == ">")  return int64_t(a > b ? 1 : 0);
            if (bin->op == ">=") return int64_t(a >= b ? 1 : 0);
            if (bin->op == "**") {
                if (b < 0) return (a == 1) ? int64_t(1) : (a == -1 ? (b & 1 ? int64_t(-1) : int64_t(1)) : int64_t(0));
                // Binary exponentiation O(log n) — avoids pathological compile times
                // for expressions like 2**1000000 appearing in const-eval contexts.
                uint64_t result = 1, base = static_cast<uint64_t>(a);
                int64_t exp = b;
                while (exp > 0) {
                    if (exp & 1) result *= base;
                    base *= base;
                    exp >>= 1;
                }
                return static_cast<int64_t>(result);
            }
            return std::nullopt;
        }
        case ASTNodeType::UNARY_EXPR: {
            auto* un = static_cast<UnaryExpr*>(e);
            auto v = evalExpr(un->operand.get());
            if (!v) return std::nullopt;
            if (un->op == "-") return -*v;
            if (un->op == "~") return ~*v;
            if (un->op == "!") return int64_t(*v == 0 ? 1 : 0);
            return std::nullopt;
        }
        case ASTNodeType::TERNARY_EXPR: {
            auto* tern = static_cast<TernaryExpr*>(e);
            auto cond = evalExpr(tern->condition.get());
            if (!cond) return std::nullopt;
            return *cond != 0 ? evalExpr(tern->thenExpr.get())
                              : evalExpr(tern->elseExpr.get());
        }
        case ASTNodeType::CALL_EXPR: {
            auto* call = static_cast<CallExpr*>(e);
            // Zero-arg constant-returning functions (classified by pre-pass).
            if (call->arguments.empty()) {
                if (optCtx_) {
                    if (auto v = optCtx_->constIntReturn(call->callee)) return *v;
                }
            }
            // Recursive @const_eval call
            const bool isConstEval = optCtx_ && optCtx_->isConstFoldable(call->callee);
            if (!isConstEval)
                return std::nullopt;
            auto declIt = functionDecls_.find(call->callee);
            if (declIt == functionDecls_.end()) return std::nullopt;
            std::vector<int64_t> callArgs;
            callArgs.reserve(call->arguments.size());
            for (auto& arg : call->arguments) {
                auto av = evalExpr(arg.get());
                if (!av) return std::nullopt;
                callArgs.push_back(*av);
            }
            return tryConstEval(declIt->second, callArgs);
        }
        case ASTNodeType::SCOPE_RESOLUTION_EXPR: {
            auto* sr = static_cast<ScopeResolutionExpr*>(e);
            const std::string fullName = sr->scopeName + "_" + sr->memberName;
            auto eit = enumConstants_.find(fullName);
            if (eit != enumConstants_.end()) return static_cast<int64_t>(eit->second);
            return std::nullopt;
        }
        default:
            return std::nullopt;
        }
    };

    evalStmt = [&](Statement* s) -> bool {
        if (!s || retVal) return true;  // already returned
        switch (s->type) {
        case ASTNodeType::RETURN_STMT: {
            auto* ret = static_cast<ReturnStmt*>(s);
            if (ret->value) {
                auto v = evalExpr(ret->value.get());
                if (!v) return false;
                retVal = v;
            } else {
                retVal = 0;
            }
            return true;
        }
        case ASTNodeType::VAR_DECL: {
            auto* decl = static_cast<VarDecl*>(s);
            if (decl->initializer) {
                auto v = evalExpr(decl->initializer.get());
                if (!v) return false;
                env[decl->name] = *v;
            } else {
                env[decl->name] = 0;
            }
            return true;
        }
        case ASTNodeType::EXPR_STMT: {
            auto* es = static_cast<ExprStmt*>(s);
            if (es->expression->type == ASTNodeType::ASSIGN_EXPR) {
                auto* assign = static_cast<AssignExpr*>(es->expression.get());
                auto v = evalExpr(assign->value.get());
                if (!v) return false;
                env[assign->name] = *v;
                return true;
            }
            // Other expression statements: just evaluate for side effects
            auto v = evalExpr(es->expression.get());
            return v.has_value();
        }
        case ASTNodeType::IF_STMT: {
            auto* ifs = static_cast<IfStmt*>(s);
            auto cond = evalExpr(ifs->condition.get());
            if (!cond) return false;
            if (*cond != 0) {
                return evalStmt(ifs->thenBranch.get());
            } else if (ifs->elseBranch) {
                return evalStmt(ifs->elseBranch.get());
            }
            return true;
        }
        case ASTNodeType::BLOCK: {
            auto* block = static_cast<BlockStmt*>(s);
            for (auto& stmt : block->statements) {
                if (!evalStmt(stmt.get())) return false;
                if (retVal) return true;
            }
            return true;
        }
        default:
            return false;  // Unsupported statement type
        }
    };

    // Evaluate the function body
    for (auto& stmt : func->body->statements) {
        if (!evalStmt(stmt.get())) return std::nullopt;
        if (retVal) return retVal;
    }
    return retVal.value_or(0);
}

// ── Compile-time evaluators — all evaluation is delegated to CF-CTRE ──────
//
// tryFoldExprToConst: fold a single expression to a compile-time constant.
// Delegates entirely to CTEngine::evalSingleExpr so that all constant-folding
// logic lives in one place (cfctre.cpp).
std::optional<CodeGenerator::ConstValue>
CodeGenerator::tryFoldExprToConst(Expression* expr, int depth) const {
    (void)depth;
    if (!expr || !ctEngine_) return std::nullopt;
    const auto env = buildComptimeEnv();
    const CTValue result = ctEngine_->evalSingleExpr(env, expr);
    if (!result.isKnown() || result.isSymbolic()) return std::nullopt;
    return ctValueToConstValue(result);
}

// tryFoldInt / tryFoldStr: convenience wrappers used by generateBuiltin.
std::optional<int64_t> CodeGenerator::tryFoldInt(Expression* e) const {
    if (!e) return std::nullopt;
    if (auto cv = tryFoldExprToConst(e))
        if (cv->kind == ConstValue::Kind::Integer) return cv->intVal;
    return std::nullopt;
}
std::optional<std::string> CodeGenerator::tryFoldStr(Expression* e) const {
    if (!e) return std::nullopt;
    if (auto cv = tryFoldExprToConst(e))
        if (cv->kind == ConstValue::Kind::String) return cv->strVal;
    return std::nullopt;
}

// tryConstEvalFull: evaluate a function body at compile time.
// Delegates to CTEngine — build a CTValue environment from argEnv and use
// evalComptimeBlock so that all evaluation logic lives in cfctre.cpp.
std::optional<CodeGenerator::ConstValue>
CodeGenerator::tryConstEvalFull(
    const FunctionDecl* func,
    const std::unordered_map<std::string, ConstValue>& argEnv,
    int depth) const {
    (void)depth;
    if (!func || !func->body || !ctEngine_) return std::nullopt;
    // Build CTValue env from the ConstValue arg map.
    std::unordered_map<std::string, CTValue> ctEnv;
    ctEnv.reserve(argEnv.size());
    for (const auto& [k, v] : argEnv)
        ctEnv[k] = constValueToCTValue(v);
    // Merge in the global comptime env (globals, enums, file-level consts).
    for (auto& [k, v] : buildComptimeEnv())
        ctEnv.emplace(k, v);  // argEnv takes precedence (already inserted above)
    auto result = ctEngine_->evalComptimeBlock(func->body.get(), ctEnv);
    if (!result || !result->isKnown() || result->isSymbolic()) return std::nullopt;
    return ctValueToConstValue(*result);
}

// BlockStmt overload: evaluate a standalone comptime block.
std::optional<CodeGenerator::ConstValue>
CodeGenerator::tryConstEvalFull(
    const BlockStmt* body,
    const std::unordered_map<std::string, ConstValue>& argEnv,
    int depth) const {
    (void)depth;
    if (!body || !ctEngine_) return std::nullopt;
    std::unordered_map<std::string, CTValue> ctEnv;
    ctEnv.reserve(argEnv.size());
    for (const auto& [k, v] : argEnv)
        ctEnv[k] = constValueToCTValue(v);
    for (auto& [k, v] : buildComptimeEnv())
        ctEnv.emplace(k, v);
    auto result = ctEngine_->evalComptimeBlock(body, ctEnv);
    if (!result || !result->isKnown() || result->isSymbolic()) return std::nullopt;
    return ctValueToConstValue(*result);
}
// Pre-pass over the program AST that identifies zero-parameter, pure functions
void CodeGenerator::analyzeConstantReturnValues(Program* program) {
    if (optimizationLevel < OptimizationLevel::O1) return;
    if (!ctEngine_) return;

    // All zero-arg constant return analysis is handled by CTEngine (runPass +
    // the back-propagation loop in runCFCTRE).  Any result not yet in optCtx_
    // can be queried directly from CTEngine here.
    for (auto& func : program->functions) {
        const std::string& fname = func->name;
        if (!func->body || !func->parameters.empty()) continue;
        if (optCtx_ && (optCtx_->constIntReturn(fname) ||
                        optCtx_->constStringReturn(fname))) continue;
        if (!ctEngine_->isPure(fname)) continue;
        auto result = ctEngine_->executeFunction(fname, {});
        if (!result || !result->isKnown() || result->isSymbolic()) continue;
        if (result->isInt()) {
            if (optCtx_) optCtx_->mutableFacts(fname).constIntReturn = result->asI64();
        } else if (result->isString()) {
            if (optCtx_) optCtx_->mutableFacts(fname).constStringReturn = result->asStr();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void CodeGenerator::runCFCTRE(Program* program) {
    if (!program) return;

    // Initialise CTEngine (may be called multiple times for repl/incremental).
    ctEngine_ = std::make_unique<CTEngine>();

    // Register global integer / string constants gathered by earlier passes.
    for (auto& [name, val] : constIntFolds_)
        ctEngine_->registerGlobalConst(name.str(), CTValue::fromI64(val));
    for (auto& [name, val] : constStringFolds_)
        ctEngine_->registerGlobalConst(name.str(), CTValue::fromString(val));
    for (auto& [name, val] : constArrayFolds_) {
        ConstValue cv = ConstValue::fromArr(val);
        ctEngine_->registerGlobalConst(name.str(), constValueToCTValue(cv));
    }

    // Register enum constants.
    for (auto& [name, val] : enumConstants_)
        ctEngine_->registerEnumConst(name.str(), static_cast<int64_t>(val));

    // Run the main CF-CTRE pass (registers all functions, computes purity,
    // pre-evaluates zero-arg pure functions, builds the call graph).
    ctEngine_->runPass(program);

    // Back-propagate CF-CTRE zero-arg results into OptimizationContext so the
    // existing tryFoldExprToConst / tryConstEvalFull helpers remain effective.
    for (auto& fn : program->functions) {
        if (!fn->parameters.empty()) continue;
        if (!ctEngine_->isPure(fn->name)) continue;
        // Query the memoised zero-arg result.
        auto result = ctEngine_->executeFunction(fn->name, {});
        if (!result) continue;
        if (result->isInt() &&
            !(optCtx_ && optCtx_->constIntReturn(fn->name))) {
            if (optCtx_) optCtx_->mutableFacts(fn->name).constIntReturn = result->asI64();
        } else if (result->isString() &&
                   !(optCtx_ && optCtx_->constStringReturn(fn->name))) {
            if (optCtx_) optCtx_->mutableFacts(fn->name).constStringReturn = result->asStr();
        }
    }

    // Back-propagate Phase 7 (uniform return values) — functions with parameters
    // that always return the same constant, proven by symbolic argument evaluation.
    for (auto& [name, ctVal] : ctEngine_->uniformReturnValues()) {
        if (ctVal.isInt() && !(optCtx_ && optCtx_->constIntReturn(name))) {
            if (optCtx_) optCtx_->mutableFacts(name).constIntReturn = ctVal.asI64();
        } else if (ctVal.isString() && !(optCtx_ && optCtx_->constStringReturn(name))) {
            if (optCtx_) optCtx_->mutableFacts(name).constStringReturn = ctVal.asStr();
        }
    }

    // Apply Phase 8 dead-function hints: mark unreachable functions as cold
    if (!ctEngine_->deadFunctions().empty()) {
        const auto& dead = ctEngine_->deadFunctions();
        for (auto& fn : program->functions) {
            if (!dead.count(fn->name)) continue;
            if (fn->hintCold) continue;   // user already annotated
            fn->hintCold    = true;
            fn->hintNoInline = true;
        }
    }

    if (verbose_) {
        const auto& s = ctEngine_->stats();
        std::cout << "  [cfctre] Pass complete: "
                  << s.functionsRegistered   << " functions registered, "
                  << s.pureFunctionsDetected << " pure, "
                  << s.functionCallsMemoized << " calls memoised, "
                  << s.arraysAllocated       << " arrays allocated, "
                  << s.loopsReasoned         << " loops reasoned, "
                  << s.branchMerges          << " branch merges, "
                  << s.ternaryMerges         << " ternary merges, "
                  << s.uniformReturnFunctionsFound << " uniform-return, "
                  << s.deadFunctionsDetected << " dead\n"
                  << "  [cfctre/absinterp] "
                  << s.deadBranchesEliminated << " dead branches, "
                  << s.safeArrayAccesses      << " safe array accesses, "
                  << s.safeDivisions          << " safe divisions, "
                  << s.safeArithmetic         << " safe arithmetic ops, "
                  << s.cheaperRewritesFound   << " cheaper rewrites" << '\n';
    }

    // ── CF-CTRE-guided inline hints ────────────────────────────────────────
    if (ctEngine_ && optimizationLevel >= OptimizationLevel::O2) {
        const auto& foldable = ctEngine_->foldableCallees();
        for (auto& fn : program->functions) {
            if (fn->hintNoInline) continue;
            if (fn->parameters.empty()) continue;   // zero-arg already folded
            if (!foldable.count(fn->name)) continue;
            // Only upgrade to hintInline if the function isn't already more
            // aggressively annotated (@flatten / alwaysInline).
            if (!fn->hintInline)
                fn->hintInline = true;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void CodeGenerator::runSynthesisPass(Program* program, bool verbose) {
    ::omscript::runSynthesisPass(program, verbose);
}

// ─────────────────────────────────────────────────────────────────────────────

void CodeGenerator::runEGraphPass(Program* program, OptimizationContext& ctx) {
    if (!enableEGraph_ || optimizationLevel < OptimizationLevel::O2) {
        if (verbose_ && optimizationLevel < OptimizationLevel::O2) {
            std::cout << "  [opt] E-graph optimization skipped (requires O2+)" << '\n';
        }
        return;
    }

    // Set per-level configuration on the subsystem before running.
    // O3 / OPTMAX get higher limits; O2 gets the conservative defaults.
    EGraphConfig cfg;
    if (optimizationLevel >= OptimizationLevel::O3) {
        cfg.maxNodes      = 50000;
        cfg.maxIterations = 30;
    } else {
        cfg.maxNodes      = 10000;
        cfg.maxIterations = 15;
    }
    cfg.enableConstFolding = true;
    ctx.egraph().setConfig(cfg);

    if (verbose_) {
        std::cout << "  [opt] Running e-graph equality saturation on AST ("
                  << program->functions.size() << " functions, maxNodes="
                  << cfg.maxNodes << ", maxIter=" << cfg.maxIterations
                  << ")..." << '\n';
    }

    // All work goes through the subsystem.
    ctx.egraph().optimizeProgram(program);

    if (verbose_) {
        const auto& s = ctx.egraph().stats();
        std::cout << "  [opt] E-graph saturation complete: "
                  << s.expressionsSimplified << "/" << s.expressionsAttempted
                  << " expressions simplified, "
                  << s.functionsChanged << " functions changed, "
                  << s.expressionsSkipped << " skipped (not representable)\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────

CodeGenerator::ConstValue CodeGenerator::ctValueToConstValue(const CTValue& v) const {
    switch (v.kind) {
    case CTValueKind::CONCRETE_I64:
    case CTValueKind::CONCRETE_U64:
    case CTValueKind::CONCRETE_BOOL:
        return ConstValue::fromInt(v.asI64());
    case CTValueKind::CONCRETE_STRING:
        return ConstValue::fromStr(v.asStr());
    case CTValueKind::CONCRETE_ARRAY: {
        if (!ctEngine_) return ConstValue{};
        auto elems = ctEngine_->extractArray(v.asArr());
        std::vector<ConstValue> cvElems;
        cvElems.reserve(elems.size());
        for (const auto& e : elems)
            cvElems.push_back(ctValueToConstValue(e));
        return ConstValue::fromArr(std::move(cvElems));
    }
    default:
        return ConstValue{};
    }
}

CTValue CodeGenerator::constValueToCTValue(const ConstValue& v) const {
    switch (v.kind) {
    case ConstValue::Kind::Integer: return CTValue::fromI64(v.intVal);
    case ConstValue::Kind::String:  return CTValue::fromString(v.strVal);
    case ConstValue::Kind::Array: {
        if (!ctEngine_) return CTValue::uninit();
        CTArrayHandle h = ctEngine_->heap().nextHandle();
        // Use the non-const heap via ctEngine_ (cast is safe — evaluation already done).
        CTHeap& hp = const_cast<CTHeap&>(ctEngine_->heap());
        h = hp.alloc(static_cast<uint64_t>(v.arrVal.size()));
        for (size_t i = 0; i < v.arrVal.size(); ++i)
            hp.store(h, static_cast<int64_t>(i),
                     constValueToCTValue(v.arrVal[i]));
        return CTValue::fromArray(h);
    }
    default:
        return CTValue::uninit();
    }
}

// buildComptimeEnv: snapshot all compile-time-known local variables into a
std::unordered_map<std::string, CTValue>
CodeGenerator::buildComptimeEnv() const {
    std::unordered_map<std::string, CTValue> env;
    if (!ctEngine_) return env;
    for (const auto& e : constIntFolds_)
        env[e.first().str()] = CTValue::fromI64(e.second);
    for (const auto& e : scopeComptimeInts_)
        env[e.first().str()] = CTValue::fromI64(e.second);
    for (const auto& e : constStringFolds_)
        env[e.first().str()] = CTValue::fromString(e.second);
    for (const auto& e : constArrayFolds_) {
        auto ctv = constValueToCTValue(ConstValue::fromArr(e.second));
        if (ctv.isKnown())
            env[e.first().str()] = std::move(ctv);
    }
    return env;
}

// autoDetectConstEvalFunctions: identify user-defined functions with parameters
void CodeGenerator::autoDetectConstEvalFunctions(Program* program) {
    if (!program || optimizationLevel < OptimizationLevel::O1) return;

    // Purity queries now delegated to the unified BuiltinEffectTable.

    // Build index of all user function declarations for O(1) lookup.
    std::unordered_map<std::string, const FunctionDecl*> allFuncs;
    for (const auto& func : program->functions) {
        allFuncs[func->name] = func.get();
    }

    // Track which user functions are currently known-pure.
    std::unordered_set<std::string> knownPure;

    // Seed with explicitly @const_eval-annotated functions (from AST) and any
    for (const auto& func : program->functions) {
        if (func->hintConstEval)
            knownPure.insert(func->name);
    }
    if (optCtx_) {
        for (const auto& [name, ff] : optCtx_->allFacts()) {
            if (ff.constIntReturn || ff.constStringReturn || ff.isConstFoldable)
                knownPure.insert(name);
        }
    }

    // Helpers to check purity of AST nodes (forward-declared via lambdas).
    std::function<bool(const Expression*)> isExprPure;
    std::function<bool(const Statement*)> isStmtPure;

    isExprPure = [&](const Expression* expr) -> bool {
        if (!expr) return true;
        switch (expr->type) {
        case ASTNodeType::LITERAL_EXPR:
        case ASTNodeType::IDENTIFIER_EXPR:
            return true;

        case ASTNodeType::BINARY_EXPR: {
            auto* bin = static_cast<const BinaryExpr*>(expr);
            return isExprPure(bin->left.get()) && isExprPure(bin->right.get());
        }
        case ASTNodeType::UNARY_EXPR: {
            auto* un = static_cast<const UnaryExpr*>(expr);
            return isExprPure(un->operand.get());
        }
        case ASTNodeType::TERNARY_EXPR: {
            auto* tern = static_cast<const TernaryExpr*>(expr);
            return isExprPure(tern->condition.get()) &&
                   isExprPure(tern->thenExpr.get()) &&
                   isExprPure(tern->elseExpr.get());
        }
        case ASTNodeType::CALL_EXPR: {
            auto* call = static_cast<const CallExpr*>(expr);
            // Impure builtins: not pure (uses unified BuiltinEffectTable).
            if (BuiltinEffectTable::isImpure(call->callee)) return false;
            // Known-pure builtins: OK (uses unified BuiltinEffectTable).
            if (BuiltinEffectTable::isPure(call->callee)) {
                for (const auto& arg : call->arguments) {
                    if (!isExprPure(arg.get())) return false;
                }
                return true;
            }
            // User-defined function: pure only if we already know it's pure.
            if (knownPure.count(call->callee)) {
                for (const auto& arg : call->arguments) {
                    if (!isExprPure(arg.get())) return false;
                }
                return true;
            }
            // Unknown/impure function.
            return false;
        }
        case ASTNodeType::MOVE_EXPR: {
            auto* mv = static_cast<const MoveExpr*>(expr);
            return isExprPure(mv->source.get());
        }
        case ASTNodeType::BORROW_EXPR: {
            auto* bw = static_cast<const BorrowExpr*>(expr);
            return isExprPure(bw->source.get());
        }
        case ASTNodeType::COMPTIME_EXPR:
            // A comptime {} block is always a compile-time constant — always pure.
            return true;
        case ASTNodeType::POSTFIX_EXPR:
            // ++/-- are mutations — not pure for const-eval.
            return false;
        case ASTNodeType::PREFIX_EXPR: {
            auto* pre = static_cast<const PrefixExpr*>(expr);
            if (pre->op == "!" || pre->op == "-" || pre->op == "+")
                return isExprPure(pre->operand.get());
            return false;  // ++/-- are mutations
        }
        default:
            // IndexExpr, AssignExpr, ArrayLiteral, etc. — conservative: not pure
            // for const-eval purposes (may allocate or mutate).
            return false;
        }
    };

    isStmtPure = [&](const Statement* stmt) -> bool {
        if (!stmt) return true;
        switch (stmt->type) {
        case ASTNodeType::RETURN_STMT: {
            auto* ret = static_cast<const ReturnStmt*>(stmt);
            return !ret->value || isExprPure(ret->value.get());
        }
        case ASTNodeType::VAR_DECL: {
            auto* vd = static_cast<const VarDecl*>(stmt);
            return !vd->initializer || isExprPure(vd->initializer.get());
        }
        case ASTNodeType::MOVE_DECL: {
            auto* md = static_cast<const MoveDecl*>(stmt);
            return !md->initializer || isExprPure(md->initializer.get());
        }
        case ASTNodeType::BLOCK: {
            auto* block = static_cast<const BlockStmt*>(stmt);
            for (const auto& s : block->statements) {
                if (!isStmtPure(s.get())) return false;
            }
            return true;
        }
        case ASTNodeType::IF_STMT: {
            auto* ifS = static_cast<const IfStmt*>(stmt);
            return isExprPure(ifS->condition.get()) &&
                   isStmtPure(ifS->thenBranch.get()) &&
                   isStmtPure(ifS->elseBranch.get());
        }
        case ASTNodeType::WHILE_STMT: {
            auto* ws = static_cast<const WhileStmt*>(stmt);
            return isExprPure(ws->condition.get()) && isStmtPure(ws->body.get());
        }
        case ASTNodeType::DO_WHILE_STMT: {
            auto* dws = static_cast<const DoWhileStmt*>(stmt);
            return isStmtPure(dws->body.get()) && isExprPure(dws->condition.get());
        }
        case ASTNodeType::FOR_STMT: {
            auto* fs = static_cast<const ForStmt*>(stmt);
            return isExprPure(fs->start.get()) &&
                   isExprPure(fs->end.get()) &&
                   (!fs->step || isExprPure(fs->step.get())) &&
                   isStmtPure(fs->body.get());
        }
        case ASTNodeType::EXPR_STMT: {
            // Allow pure expression statements (e.g. function calls used for
            // their return value stored in a variable).
            auto* es = static_cast<const ExprStmt*>(stmt);
            return isExprPure(es->expression.get());
        }
        case ASTNodeType::INVALIDATE_STMT:
            // invalidate frees memory — conservative: not pure.
            return false;
        case ASTNodeType::BREAK_STMT:
        case ASTNodeType::CONTINUE_STMT:
            return true;
        case ASTNodeType::SWITCH_STMT: {
            auto* sw = static_cast<const SwitchStmt*>(stmt);
            if (!isExprPure(sw->condition.get())) return false;
            for (const auto& c : sw->cases) {
                if (c.value && !isExprPure(c.value.get())) return false;
                for (const auto& v : c.values)
                    if (!isExprPure(v.get())) return false;
                for (const auto& s : c.body)
                    if (!isStmtPure(s.get())) return false;
            }
            return true;
        }
        default:
            return false;
        }
    };

    // Fixed-point iteration: keep adding functions to knownPure until no more
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& func : program->functions) {
            const std::string& fname = func->name;
            // Skip functions already known pure.
            if (knownPure.count(fname)) continue;
            // Must have a body to analyze.
            if (!func->body) continue;
            // Must have at least one parameter (zero-arg handled separately).
            if (func->parameters.empty()) continue;
            // Skip functions annotated @const_eval already (handled at registration time).
            if (func->hintConstEval) continue;
            // Skip functions with @optnone or @noinline.
            if (func->hintOptNone) continue;

            // Check if the entire body is pure.
            bool bodyIsPure = true;
            for (const auto& stmt : func->body->statements) {
                if (!isStmtPure(stmt.get())) {
                    bodyIsPure = false;
                    break;
                }
            }

            if (bodyIsPure) {
                knownPure.insert(fname);
                if (optCtx_) optCtx_->mutableFacts(fname).isConstFoldable = true;
                changed = true;
            }
        }
    }
}

// inferFunctionEffects: AST-level side-effect analysis (overhauled).
void CodeGenerator::inferFunctionEffects(Program* program) {
    if (!program) return;

    // ── Index user functions and globals ──────────────────────────────────
    std::unordered_map<std::string, const FunctionDecl*> allFuncs;
    std::unordered_map<std::string, std::size_t> paramIndex;  // param name -> index, per current function
    for (const auto& f : program->functions)
        allFuncs[f->name] = f.get();

    std::unordered_set<std::string> globalNames;
    for (const auto& g : program->globals)
        if (g) globalNames.insert(g->name);

    // ── Working effect summaries ──────────────────────────────────────────
    std::unordered_map<std::string, FunctionEffects> funcEffects;
    for (const auto& f : program->functions) {
        FunctionEffects fx;
        fx.paramMutated.assign(f->parameters.size(), false);
        funcEffects[f->name] = std::move(fx);
    }

    // Helper: when an expression is an identifier, return its name; otherwise "".
    auto identName = [](const Expression* e) -> std::string {
        if (!e) return {};
        if (e->type == ASTNodeType::IDENTIFIER_EXPR)
            return static_cast<const IdentifierExpr*>(e)->name;
        return {};
    };

    // ── Visitors (forward-declared so they can recurse mutually) ──────────
    std::function<FunctionEffects(const Expression*, const FunctionDecl*)> exprEffects;
    std::function<FunctionEffects(const Statement*,  const FunctionDecl*)> stmtEffects;

    // Mark target name as a parameter mutation if the name resolves to a
    auto markNameStore = [&](const std::string& name, const FunctionDecl* self,
                             FunctionEffects& fx) {
        if (!self) return;
        for (std::size_t i = 0; i < self->parameters.size(); ++i) {
            if (self->parameters[i].name == name) {
                if (fx.paramMutated.size() != self->parameters.size())
                    fx.paramMutated.assign(self->parameters.size(), false);
                fx.paramMutated[i] = true;
                fx.hasMutation = true;
                fx.writesMemory = true;
                return;
            }
        }
        if (globalNames.count(name)) {
            fx.writesGlobal = true;
        }
        // Else: local variable assignment — no observable effect.
    };

    auto markNameLoad = [&](const std::string& name, const FunctionDecl*,
                            FunctionEffects& fx) {
        if (globalNames.count(name)) fx.readsGlobal = true;
    };

    exprEffects = [&](const Expression* expr, const FunctionDecl* self) -> FunctionEffects {
        FunctionEffects fx;
        if (self) fx.paramMutated.assign(self->parameters.size(), false);
        if (!expr) return fx;
        switch (expr->type) {
        case ASTNodeType::IDENTIFIER_EXPR: {
            auto* id = static_cast<const IdentifierExpr*>(expr);
            markNameLoad(id->name, self, fx);
            break;
        }
        case ASTNodeType::INDEX_EXPR:
            fx.readsMemory = true;
            break;
        case ASTNodeType::INDEX_ASSIGN_EXPR: {
            auto* ia = static_cast<const IndexAssignExpr*>(expr);
            fx.writesMemory = true;
            fx.hasMutation  = true;
            // If the array root is a parameter, attribute mutation to it.
            // Walk through INDEX_EXPR / FIELD_ACCESS_EXPR chains to find root.
            const Expression* root = ia->array.get();
            while (root) {
                if (root->type == ASTNodeType::INDEX_EXPR) {
                    root = static_cast<const IndexExpr*>(root)->array.get();
                } else if (root->type == ASTNodeType::FIELD_ACCESS_EXPR) {
                    root = static_cast<const FieldAccessExpr*>(root)->object.get();
                } else {
                    break;
                }
            }
            if (root && self) {
                std::string n = identName(root);
                if (!n.empty()) {
                    for (std::size_t i = 0; i < self->parameters.size(); ++i) {
                        if (self->parameters[i].name == n) {
                            fx.paramMutated[i] = true;
                            break;
                        }
                    }
                    if (globalNames.count(n)) fx.writesGlobal = true;
                }
            }
            fx.mergeFrom(exprEffects(ia->array.get(), self));
            fx.mergeFrom(exprEffects(ia->index.get(), self));
            fx.mergeFrom(exprEffects(ia->value.get(), self));
            break;
        }
        case ASTNodeType::FIELD_ACCESS_EXPR: {
            auto* fa = static_cast<const FieldAccessExpr*>(expr);
            fx.readsMemory = true;
            fx.mergeFrom(exprEffects(fa->object.get(), self));
            break;
        }
        case ASTNodeType::FIELD_ASSIGN_EXPR: {
            auto* fas = static_cast<const FieldAssignExpr*>(expr);
            fx.writesMemory = true;
            fx.hasMutation  = true;
            // Walk to root (similar to index assign).
            const Expression* root = fas->object.get();
            while (root) {
                if (root->type == ASTNodeType::FIELD_ACCESS_EXPR) {
                    root = static_cast<const FieldAccessExpr*>(root)->object.get();
                } else if (root->type == ASTNodeType::INDEX_EXPR) {
                    root = static_cast<const IndexExpr*>(root)->array.get();
                } else {
                    break;
                }
            }
            if (root && self) {
                std::string n = identName(root);
                if (!n.empty()) {
                    for (std::size_t i = 0; i < self->parameters.size(); ++i) {
                        if (self->parameters[i].name == n) {
                            fx.paramMutated[i] = true;
                            break;
                        }
                    }
                    if (globalNames.count(n)) fx.writesGlobal = true;
                }
            }
            fx.mergeFrom(exprEffects(fas->object.get(), self));
            fx.mergeFrom(exprEffects(fas->value.get(), self));
            break;
        }
        case ASTNodeType::CALL_EXPR: {
            auto* call = static_cast<const CallExpr*>(expr);
            const auto& bt = BuiltinEffectTable::get(call->callee);
            const bool isKnownBuiltin =
                bt.constFoldable || bt.readsMemory || bt.writesMemory || bt.hasIO
                || bt.mayThrow   || bt.noReturn   || bt.allocates    || bt.deallocates;
            const bool isUserFunc = allFuncs.count(call->callee) != 0;

            if (isKnownBuiltin) {
                if (bt.hasIO)        fx.hasIO        = true;
                if (bt.readsMemory)  fx.readsMemory  = true;
                if (bt.writesMemory) { fx.writesMemory = true; fx.hasMutation = true; }
                if (bt.mayThrow)     fx.mayThrow     = true;
                if (bt.noReturn)     fx.mayNotReturn = true;
                if (bt.allocates)    fx.allocates    = true;
                if (bt.deallocates)  fx.deallocates  = true;
                // Mutating builtins (push/pop/sort/...) mutate their first
                // argument — propagate per-parameter mutation when possible.
                if (bt.writesMemory && !call->arguments.empty() && self) {
                    std::string n = identName(call->arguments.front().get());
                    if (!n.empty()) {
                        for (std::size_t i = 0; i < self->parameters.size(); ++i) {
                            if (self->parameters[i].name == n) {
                                fx.paramMutated[i] = true;
                                break;
                            }
                        }
                        if (globalNames.count(n)) fx.writesGlobal = true;
                    }
                }
            } else if (isUserFunc) {
                // Propagate callee effects from the working summary table.
                auto it = funcEffects.find(call->callee);
                if (it != funcEffects.end()) {
                    const FunctionEffects& cf = it->second;
                    fx.readsMemory     = fx.readsMemory     || cf.readsMemory;
                    fx.writesMemory    = fx.writesMemory    || cf.writesMemory;
                    fx.hasIO           = fx.hasIO           || cf.hasIO;
                    fx.hasMutation     = fx.hasMutation     || cf.hasMutation;
                    fx.mayThrow        = fx.mayThrow        || cf.mayThrow;
                    fx.mayNotReturn    = fx.mayNotReturn    || cf.mayNotReturn;
                    fx.allocates       = fx.allocates       || cf.allocates;
                    fx.deallocates     = fx.deallocates     || cf.deallocates;
                    fx.hasIndirectCall = fx.hasIndirectCall || cf.hasIndirectCall;
                    fx.readsGlobal     = fx.readsGlobal     || cf.readsGlobal;
                    fx.writesGlobal    = fx.writesGlobal    || cf.writesGlobal;
                    // If the callee mutates its parameter i, and we passed a
                    if (self) {
                        std::size_t n = std::min(cf.paramMutated.size(),
                                                 call->arguments.size());
                        for (std::size_t i = 0; i < n; ++i) {
                            if (!cf.paramMutated[i]) continue;
                            std::string an = identName(call->arguments[i].get());
                            if (an.empty()) continue;
                            for (std::size_t p = 0; p < self->parameters.size(); ++p) {
                                if (self->parameters[p].name == an) {
                                    fx.paramMutated[p] = true;
                                    break;
                                }
                            }
                            if (globalNames.count(an)) fx.writesGlobal = true;
                        }
                    }
                }
                // Honour @noreturn annotation on the callee.
                auto fdIt = allFuncs.find(call->callee);
                if (fdIt != allFuncs.end() && fdIt->second && fdIt->second->hintNoReturn)
                    fx.mayNotReturn = true;
            } else {
                // Unknown callee — pessimise every axis.
                fx.readsMemory     = true;
                fx.writesMemory    = true;
                fx.hasIO           = true;
                fx.hasMutation     = true;
                fx.mayThrow        = true;
                fx.mayNotReturn    = true;
                fx.allocates       = true;
                fx.deallocates     = true;
                fx.hasIndirectCall = true;
            }
            for (const auto& arg : call->arguments)
                fx.mergeFrom(exprEffects(arg.get(), self));
            break;
        }
        case ASTNodeType::BINARY_EXPR: {
            auto* b = static_cast<const BinaryExpr*>(expr);
            fx.mergeFrom(exprEffects(b->left.get(),  self));
            fx.mergeFrom(exprEffects(b->right.get(), self));
            // Division/modulo by a non-constant or zero divisor emits a
            if (b->op == "/" || b->op == "%") {
                auto* lit = dynamic_cast<const LiteralExpr*>(b->right.get());
                bool divisorIsNonZeroConst = lit &&
                    lit->literalType == LiteralExpr::LiteralType::INTEGER &&
                    lit->intValue != 0;
                if (!divisorIsNonZeroConst) {
                    fx.mayThrow     = true;
                    fx.mayNotReturn = true;
                    // Preserve old behaviour for callers that gate on hasIO
                    // when deciding readnone/readonly: the puts() call is I/O.
                    fx.hasIO = true;
                }
            }
            break;
        }
        case ASTNodeType::ASSIGN_EXPR: {
            auto* a = static_cast<const AssignExpr*>(expr);
            // Only assignment to a parameter or global is an observable
            // memory effect — local variable assignment is invisible.
            markNameStore(a->name, self, fx);
            fx.mergeFrom(exprEffects(a->value.get(), self));
            break;
        }
        case ASTNodeType::UNARY_EXPR: {
            auto* u = static_cast<const UnaryExpr*>(expr);
            fx.mergeFrom(exprEffects(u->operand.get(), self));
            break;
        }
        case ASTNodeType::POSTFIX_EXPR: {
            auto* p = static_cast<const PostfixExpr*>(expr);
            // ++/-- mutate the operand iff it's a parameter or global.
            std::string n = identName(p->operand.get());
            if (!n.empty()) markNameStore(n, self, fx);
            else fx.mergeFrom(exprEffects(p->operand.get(), self));
            break;
        }
        case ASTNodeType::PREFIX_EXPR: {
            auto* p = static_cast<const PrefixExpr*>(expr);
            std::string n = identName(p->operand.get());
            if (!n.empty()) markNameStore(n, self, fx);
            else fx.mergeFrom(exprEffects(p->operand.get(), self));
            break;
        }
        case ASTNodeType::TERNARY_EXPR: {
            auto* t = static_cast<const TernaryExpr*>(expr);
            fx.mergeFrom(exprEffects(t->condition.get(), self));
            fx.mergeFrom(exprEffects(t->thenExpr.get(),  self));
            fx.mergeFrom(exprEffects(t->elseExpr.get(),  self));
            break;
        }
        case ASTNodeType::ARRAY_EXPR: {
            auto* arr = static_cast<const ArrayExpr*>(expr);
            // Array literal allocates a new array on the heap.
            fx.allocates = true;
            for (const auto& el : arr->elements)
                fx.mergeFrom(exprEffects(el.get(), self));
            break;
        }
        case ASTNodeType::STRUCT_LITERAL_EXPR: {
            auto* sl = static_cast<const StructLiteralExpr*>(expr);
            for (const auto& fv : sl->fieldValues)
                fx.mergeFrom(exprEffects(fv.second.get(), self));
            break;
        }
        default:
            // Literals, move/borrow/freeze — no effects.
            break;
        }
        return fx;
    };

    stmtEffects = [&](const Statement* stmt, const FunctionDecl* self) -> FunctionEffects {
        FunctionEffects fx;
        if (self) fx.paramMutated.assign(self->parameters.size(), false);
        if (!stmt) return fx;

        switch (stmt->type) {
        case ASTNodeType::EXPR_STMT:
            fx.mergeFrom(exprEffects(static_cast<const ExprStmt*>(stmt)->expression.get(), self));
            break;
        case ASTNodeType::VAR_DECL: {
            auto* vd = static_cast<const VarDecl*>(stmt);
            if (vd->initializer) fx.mergeFrom(exprEffects(vd->initializer.get(), self));
            break;
        }
        case ASTNodeType::MOVE_DECL: {
            auto* md = static_cast<const MoveDecl*>(stmt);
            if (md->initializer) fx.mergeFrom(exprEffects(md->initializer.get(), self));
            break;
        }
        case ASTNodeType::RETURN_STMT: {
            auto* ret = static_cast<const ReturnStmt*>(stmt);
            if (ret->value) fx.mergeFrom(exprEffects(ret->value.get(), self));
            break;
        }
        case ASTNodeType::BLOCK: {
            for (const auto& s : static_cast<const BlockStmt*>(stmt)->statements)
                fx.mergeFrom(stmtEffects(s.get(), self));
            break;
        }
        case ASTNodeType::IF_STMT: {
            auto* ifs = static_cast<const IfStmt*>(stmt);
            fx.mergeFrom(exprEffects(ifs->condition.get(),  self));
            fx.mergeFrom(stmtEffects(ifs->thenBranch.get(), self));
            fx.mergeFrom(stmtEffects(ifs->elseBranch.get(), self));
            break;
        }
        case ASTNodeType::WHILE_STMT: {
            auto* ws = static_cast<const WhileStmt*>(stmt);
            fx.mergeFrom(exprEffects(ws->condition.get(), self));
            fx.mergeFrom(stmtEffects(ws->body.get(),      self));
            // A `while (true)` with no break is a non-terminating loop.
            if (auto* lit = dynamic_cast<const LiteralExpr*>(ws->condition.get())) {
                if (lit->literalType == LiteralExpr::LiteralType::INTEGER && lit->intValue != 0)
                    fx.mayNotReturn = true;
            }
            break;
        }
        case ASTNodeType::DO_WHILE_STMT: {
            auto* dw = static_cast<const DoWhileStmt*>(stmt);
            fx.mergeFrom(stmtEffects(dw->body.get(),      self));
            fx.mergeFrom(exprEffects(dw->condition.get(), self));
            break;
        }
        case ASTNodeType::FOR_STMT: {
            auto* fs = static_cast<const ForStmt*>(stmt);
            if (fs->start) fx.mergeFrom(exprEffects(fs->start.get(), self));
            if (fs->end)   fx.mergeFrom(exprEffects(fs->end.get(),   self));
            if (fs->step)  fx.mergeFrom(exprEffects(fs->step.get(),  self));
            fx.mergeFrom(stmtEffects(fs->body.get(), self));
            break;
        }
        case ASTNodeType::FOR_EACH_STMT: {
            auto* fe = static_cast<const ForEachStmt*>(stmt);
            fx.mergeFrom(exprEffects(fe->collection.get(), self));
            fx.mergeFrom(stmtEffects(fe->body.get(),       self));
            // Iterating an array reads it.
            fx.readsMemory = true;
            break;
        }
        case ASTNodeType::SWITCH_STMT: {
            auto* sw = static_cast<const SwitchStmt*>(stmt);
            fx.mergeFrom(exprEffects(sw->condition.get(), self));
            for (const auto& c : sw->cases) {
                if (c.value) fx.mergeFrom(exprEffects(c.value.get(), self));
                for (const auto& s : c.body) fx.mergeFrom(stmtEffects(s.get(), self));
            }
            break;
        }
        case ASTNodeType::CATCH_STMT: {
            auto* cs = static_cast<const CatchStmt*>(stmt);
            fx.mergeFrom(stmtEffects(cs->body.get(), self));
            break;
        }
        case ASTNodeType::THROW_STMT: {
            auto* th = static_cast<const ThrowStmt*>(stmt);
            if (th->value) fx.mergeFrom(exprEffects(th->value.get(), self));
            // throw is an observable, possibly-non-returning effect.  It's
            fx.hasIO        = true;
            fx.mayThrow     = true;
            fx.mayNotReturn = true;
            break;
        }
        case ASTNodeType::PIPELINE_STMT: {
            auto* pl = static_cast<const PipelineStmt*>(stmt);
            if (pl->count) fx.mergeFrom(exprEffects(pl->count.get(), self));
            for (const auto& stage : pl->stages)
                fx.mergeFrom(stmtEffects(stage.body.get(), self));
            break;
        }
        case ASTNodeType::DEFER_STMT:
            fx.mergeFrom(stmtEffects(static_cast<const DeferStmt*>(stmt)->body.get(), self));
            break;
        default:
            break;
        }
        return fx;
    };

    // ── Fixed-point iteration over the call graph ─────────────────────────
    const std::size_t kMaxIters =
        std::max<std::size_t>(8, program->functions.size() * 16);
    bool changed = true;
    std::size_t iter = 0;
    while (changed && iter++ < kMaxIters) {
        changed = false;
        for (const auto& f : program->functions) {
            if (!f->body) continue;
            FunctionEffects computed;
            computed.paramMutated.assign(f->parameters.size(), false);
            for (const auto& s : f->body->statements)
                computed.mergeFrom(stmtEffects(s.get(), f.get()));

            // Honour @noreturn annotation on self.
            if (f->hintNoReturn) computed.mayNotReturn = true;

            FunctionEffects& prev = funcEffects[f->name];
            if (!computed.equalsForFixedPoint(prev)) {
                prev = std::move(computed);
                changed = true;
            }
        }
    }

    // ── Publish to the OptimizationContext ────────────────────────────────
    if (optCtx_) {
        for (const auto& kv : funcEffects) {
            optCtx_->mutableFacts(kv.first).effects = kv.second;
        }
    }

    // ── Diagnostics: warn on @pure mismatch / count tagging opportunities ─
    for (const auto& f : program->functions) {
        auto it = funcEffects.find(f->name);
        if (it == funcEffects.end()) continue;
        const FunctionEffects& fx = it->second;
        const bool actuallyPure = fx.isReadNone() && !fx.hasIO && !fx.hasMutation;

        if (f->hintPure) {
            if (fx.hasIO) {
                std::cerr << "[warning] @pure function '" << f->name
                          << "' performs I/O — @pure annotation may be incorrect\n";
                ++optStats_.almostPureFns;
            } else if (fx.writesMemory || fx.hasMutation || fx.writesGlobal) {
                std::cerr << "[warning] @pure function '" << f->name
                          << "' mutates memory — @pure annotation may be incorrect\n";
                ++optStats_.almostPureFns;
            }
        } else if (actuallyPure && !f->parameters.empty()) {
            ++optStats_.untaggedPureFns;
        }
    }
}


} // namespace omscript

namespace omscript {
// maintaining a pool of interned global string constants.  When the
llvm::GlobalVariable* CodeGenerator::internString(const std::string& content) {
    auto it = internedStrings_.find(content);
    if (it != internedStrings_.end()) {
        return it->second;
    }

    // Create a new global string constant with internal linkage and
    // unnamed_addr so LLVM can merge it with other identical constants.
    auto* strConst = llvm::ConstantDataArray::getString(*context, content, /*AddNull=*/true);
    auto* gv = new llvm::GlobalVariable(
        *module,
        strConst->getType(),
        /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage,
        strConst,
        ".str.intern");
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    gv->setAlignment(llvm::Align(1));

    internedStrings_[content] = gv;
    return gv;
}

llvm::Value* CodeGenerator::emitComptimeArray(const std::vector<ConstValue>& elems) {
    // Build [N+1 x i64] constant with OmScript's array layout:
    auto* i64Ty = llvm::Type::getInt64Ty(*context);
    size_t N = elems.size();
    std::vector<llvm::Constant*> vals;
    vals.reserve(N + 1);
    vals.push_back(llvm::ConstantInt::get(i64Ty, static_cast<int64_t>(N)));
    for (const auto& elem : elems) {
        // Only integer elements are supported in comptime arrays for now;
        // non-integer elements are clamped to 0 rather than failing hard.
        int64_t v = (elem.kind == ConstValue::Kind::Integer) ? elem.intVal : 0;
        vals.push_back(llvm::ConstantInt::get(i64Ty, v));
    }
    auto* arrTy = llvm::ArrayType::get(i64Ty, N + 1);
    auto* arrConst = llvm::ConstantArray::get(arrTy, vals);
    auto* gv = new llvm::GlobalVariable(
        *module, arrTy, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, arrConst, ".comptime.arr");
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    gv->setAlignment(llvm::Align(8));
    return builder->CreatePtrToInt(gv, i64Ty, "comptime.arr");
}

void CodeGenerator::generateAssume(AssumeStmt* stmt) {
    llvm::Value* cond = generateExpression(stmt->condition.get());
    // Convert to i1 if needed
    if (!cond->getType()->isIntegerTy(1)) {
        cond = builder->CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0), "assume.cond");
    }
    llvm::Function* assumeFn = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::assume, {});
    builder->CreateCall(assumeFn, {cond});

    if (stmt->deoptBody) {
        // if (!cond) { deoptBody }
        llvm::Function* parent = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* deoptBB = llvm::BasicBlock::Create(*context, "deopt", parent);
        llvm::BasicBlock* afterBB = llvm::BasicBlock::Create(*context, "after.assume", parent);
        builder->CreateCondBr(cond, afterBB, deoptBB);
        builder->SetInsertPoint(deoptBB);
        generateStatement(stmt->deoptBody.get());
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(afterBB);
        }
        builder->SetInsertPoint(afterBB);
    }
}

// ---------------------------------------------------------------------------

/// Scan an expression tree for any use of varName that would cause escape:
static bool exprUsesVar(Expression* expr, const std::string& varName) {
    if (!expr) return false;
    if (expr->type == ASTNodeType::IDENTIFIER_EXPR) {
        return static_cast<IdentifierExpr*>(expr)->name == varName;
    }
    if (expr->type == ASTNodeType::CALL_EXPR) {
        auto* call = static_cast<CallExpr*>(expr);
        for (auto& arg : call->arguments) {
            if (exprUsesVar(arg.get(), varName)) return true;
        }
        return false; // callee name is a string, not an expression
    }
    if (expr->type == ASTNodeType::BINARY_EXPR) {
        auto* bin = static_cast<BinaryExpr*>(expr);
        return exprUsesVar(bin->left.get(), varName) ||
               exprUsesVar(bin->right.get(), varName);
    }
    if (expr->type == ASTNodeType::UNARY_EXPR) {
        return exprUsesVar(static_cast<UnaryExpr*>(expr)->operand.get(), varName);
    }
    if (expr->type == ASTNodeType::INDEX_EXPR) {
        auto* idx = static_cast<IndexExpr*>(expr);
        return exprUsesVar(idx->array.get(), varName) ||
               exprUsesVar(idx->index.get(), varName);
    }
    if (expr->type == ASTNodeType::ASSIGN_EXPR) {
        auto* asgn = static_cast<AssignExpr*>(expr);
        return exprUsesVar(asgn->value.get(), varName);
    }
    if (expr->type == ASTNodeType::RANGE_ANNOT_EXPR) {
        return exprUsesVar(static_cast<RangeAnnotExpr*>(expr)->inner.get(), varName);
    }
    return false;
}

/// Returns true if varName escapes in any statement in the block (from stmtIdx onwards).
static bool varEscapesInBlock(const BlockStmt* block, const std::string& varName,
                               size_t startIdx);

/// Returns true if varName appears in a position that causes escape within
/// any nested statement (if/while/for bodies, etc.)
static bool varEscapesInStmt(const Statement* s, const std::string& varName) {
    if (!s) return false;
    // Return: escapes if returned
    if (auto* ret = dynamic_cast<const ReturnStmt*>(s)) {
        return ret->value && exprUsesVar(ret->value.get(), varName);
    }
    // VarDecl initializer: could be assigned to another variable and passed
    if (auto* vd = dynamic_cast<const VarDecl*>(s)) {
        return vd->initializer && exprUsesVar(vd->initializer.get(), varName);
    }
    // ExprStmt: call args or assignment
    if (auto* es = dynamic_cast<const ExprStmt*>(s)) {
        if (exprUsesVar(es->expression.get(), varName)) {
            if (es->expression->type == ASTNodeType::CALL_EXPR) {
                auto* call = static_cast<CallExpr*>(es->expression.get());
                for (auto& arg : call->arguments) {
                    if (exprUsesVar(arg.get(), varName)) return true;
                }
            }
            // Assignment of varName to another variable escapes it
            if (es->expression->type == ASTNodeType::ASSIGN_EXPR) {
                auto* asgn = static_cast<AssignExpr*>(es->expression.get());
                if (exprUsesVar(asgn->value.get(), varName)) return true;
            }
        }
    }
    // Recurse into nested control flow so uses inside if/while/for are found.
    if (auto* blk = dynamic_cast<const BlockStmt*>(s)) {
        return varEscapesInBlock(blk, varName, 0);
    }
    if (auto* ifs = dynamic_cast<const IfStmt*>(s)) {
        return varEscapesInStmt(ifs->thenBranch.get(), varName) ||
               varEscapesInStmt(ifs->elseBranch.get(), varName);
    }
    if (auto* ws = dynamic_cast<const WhileStmt*>(s)) {
        return exprUsesVar(ws->condition.get(), varName) ||
               varEscapesInStmt(ws->body.get(), varName);
    }
    if (auto* dws = dynamic_cast<const DoWhileStmt*>(s)) {
        return exprUsesVar(dws->condition.get(), varName) ||
               varEscapesInStmt(dws->body.get(), varName);
    }
    if (auto* fs = dynamic_cast<const ForStmt*>(s)) {
        return varEscapesInStmt(fs->body.get(), varName);
    }
    if (auto* fes = dynamic_cast<const ForEachStmt*>(s)) {
        return varEscapesInStmt(fes->body.get(), varName);
    }
    return false;
}

static bool varEscapesInBlock(const BlockStmt* block, const std::string& varName,
                               size_t startIdx) {
    for (size_t i = startIdx; i < block->statements.size(); ++i) {
        if (varEscapesInStmt(block->statements[i].get(), varName)) return true;
    }
    return false;
}

bool CodeGenerator::doesVarEscapeCurrentScope(const std::string& varName) const {
    if (!currentFuncDecl_ || !currentFuncDecl_->body) return true; // conservative
    // Search all scopes in the function body for the VarDecl of this variable,
    // then check all subsequent statements (including nested blocks) for escape.
    const BlockStmt* body = currentFuncDecl_->body.get();

    // Helper: recursively find the block containing the VarDecl.
    // Returns {block, decl_index} if found, or nullopt.
    std::function<std::optional<std::pair<const BlockStmt*, size_t>>(const BlockStmt*)>
        findDecl = [&](const BlockStmt* blk)
            -> std::optional<std::pair<const BlockStmt*, size_t>> {
        for (size_t i = 0; i < blk->statements.size(); ++i) {
            const Statement* s = blk->statements[i].get();
            if (auto* vd = dynamic_cast<const VarDecl*>(s)) {
                if (vd->name == varName)
                    return std::make_pair(blk, i);
            }
            // Search inside nested blocks.
            if (auto* nested = dynamic_cast<const BlockStmt*>(s)) {
                if (auto r = findDecl(nested)) return r;
            }
            if (auto* ifs = dynamic_cast<const IfStmt*>(s)) {
                if (ifs->thenBranch) {
                    if (auto* tb = dynamic_cast<const BlockStmt*>(ifs->thenBranch.get()))
                        if (auto r = findDecl(tb)) return r;
                }
                if (ifs->elseBranch) {
                    if (auto* eb = dynamic_cast<const BlockStmt*>(ifs->elseBranch.get()))
                        if (auto r = findDecl(eb)) return r;
                }
            }
            if (auto* ws = dynamic_cast<const WhileStmt*>(s)) {
                if (auto* wb = dynamic_cast<const BlockStmt*>(ws->body.get()))
                    if (auto r = findDecl(wb)) return r;
            }
            if (auto* fs = dynamic_cast<const ForStmt*>(s)) {
                if (auto* fb = dynamic_cast<const BlockStmt*>(fs->body.get()))
                    if (auto r = findDecl(fb)) return r;
            }
            if (auto* fes = dynamic_cast<const ForEachStmt*>(s)) {
                if (auto* feb = dynamic_cast<const BlockStmt*>(fes->body.get()))
                    if (auto r = findDecl(feb)) return r;
            }
        }
        return {};
    };

    if (auto found = findDecl(body)) {
        const BlockStmt* declBlock = found->first;
        const size_t declIdx = found->second;
        // Check the rest of the declaring block AND the enclosing function body
        // for any escaping use after the declaration.
        if (varEscapesInBlock(declBlock, varName, declIdx + 1)) return true;
        // If the var was declared in a nested block, also check the rest of
        if (declBlock != body) {
            return varEscapesInBlock(body, varName, 0);
        }
        return false;
    }
    // Not found: conservative.
    return true;
}

// ---------------------------------------------------------------------------

/// Forward decl: scan a statement subtree for an IndexAssign targeting varName.
static bool exprHasIndexAssignToVar(const Expression* expr, const std::string& varName);
static bool stmtHasIndexAssignToVar(const Statement* s, const std::string& varName);

static bool exprHasIndexAssignToVar(const Expression* expr, const std::string& varName) {
    if (!expr) return false;
    switch (expr->type) {
    case ASTNodeType::INDEX_ASSIGN_EXPR: {
        auto* ia = static_cast<const IndexAssignExpr*>(expr);
        // Direct target: `varName[i] = v` — the array operand is an identifier
        // referring to varName.
        if (ia->array && ia->array->type == ASTNodeType::IDENTIFIER_EXPR) {
            const auto* id = static_cast<const IdentifierExpr*>(ia->array.get());
            if (id->name == varName) return true;
        }
        // Recurse into sub-expressions.
        return exprHasIndexAssignToVar(ia->array.get(), varName) ||
               exprHasIndexAssignToVar(ia->index.get(), varName) ||
               exprHasIndexAssignToVar(ia->value.get(), varName);
    }
    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<const BinaryExpr*>(expr);
        return exprHasIndexAssignToVar(bin->left.get(), varName) ||
               exprHasIndexAssignToVar(bin->right.get(), varName);
    }
    case ASTNodeType::UNARY_EXPR:
        return exprHasIndexAssignToVar(
            static_cast<const UnaryExpr*>(expr)->operand.get(), varName);
    case ASTNodeType::PREFIX_EXPR:
        return exprHasIndexAssignToVar(
            static_cast<const PrefixExpr*>(expr)->operand.get(), varName);
    case ASTNodeType::POSTFIX_EXPR:
        return exprHasIndexAssignToVar(
            static_cast<const PostfixExpr*>(expr)->operand.get(), varName);
    case ASTNodeType::TERNARY_EXPR: {
        auto* t = static_cast<const TernaryExpr*>(expr);
        return exprHasIndexAssignToVar(t->condition.get(), varName) ||
               exprHasIndexAssignToVar(t->thenExpr.get(), varName) ||
               exprHasIndexAssignToVar(t->elseExpr.get(), varName);
    }
    case ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<const CallExpr*>(expr);
        for (const auto& arg : call->arguments) {
            if (exprHasIndexAssignToVar(arg.get(), varName)) return true;
        }
        return false;
    }
    case ASTNodeType::INDEX_EXPR: {
        auto* idx = static_cast<const IndexExpr*>(expr);
        return exprHasIndexAssignToVar(idx->array.get(), varName) ||
               exprHasIndexAssignToVar(idx->index.get(), varName);
    }
    case ASTNodeType::ASSIGN_EXPR: {
        auto* asgn = static_cast<const AssignExpr*>(expr);
        return exprHasIndexAssignToVar(asgn->value.get(), varName);
    }
    case ASTNodeType::ARRAY_EXPR: {
        auto* arr = static_cast<const ArrayExpr*>(expr);
        for (const auto& e : arr->elements) {
            if (exprHasIndexAssignToVar(e.get(), varName)) return true;
        }
        return false;
    }
    default:
        return false;
    }
}

static bool stmtHasIndexAssignToVar(const Statement* s, const std::string& varName) {
    if (!s) return false;
    if (auto* es = dynamic_cast<const ExprStmt*>(s))
        return exprHasIndexAssignToVar(es->expression.get(), varName);
    if (auto* vd = dynamic_cast<const VarDecl*>(s))
        return vd->initializer && exprHasIndexAssignToVar(vd->initializer.get(), varName);
    if (auto* ret = dynamic_cast<const ReturnStmt*>(s))
        return ret->value && exprHasIndexAssignToVar(ret->value.get(), varName);
    if (auto* blk = dynamic_cast<const BlockStmt*>(s)) {
        for (const auto& st : blk->statements)
            if (stmtHasIndexAssignToVar(st.get(), varName)) return true;
        return false;
    }
    if (auto* ifs = dynamic_cast<const IfStmt*>(s))
        return exprHasIndexAssignToVar(ifs->condition.get(), varName) ||
               stmtHasIndexAssignToVar(ifs->thenBranch.get(), varName) ||
               stmtHasIndexAssignToVar(ifs->elseBranch.get(), varName);
    if (auto* ws = dynamic_cast<const WhileStmt*>(s))
        return exprHasIndexAssignToVar(ws->condition.get(), varName) ||
               stmtHasIndexAssignToVar(ws->body.get(), varName);
    if (auto* dws = dynamic_cast<const DoWhileStmt*>(s))
        return exprHasIndexAssignToVar(dws->condition.get(), varName) ||
               stmtHasIndexAssignToVar(dws->body.get(), varName);
    if (auto* fs = dynamic_cast<const ForStmt*>(s))
        return exprHasIndexAssignToVar(fs->start.get(), varName) ||
               exprHasIndexAssignToVar(fs->end.get(), varName) ||
               (fs->step && exprHasIndexAssignToVar(fs->step.get(), varName)) ||
               stmtHasIndexAssignToVar(fs->body.get(), varName);
    if (auto* fes = dynamic_cast<const ForEachStmt*>(s))
        return exprHasIndexAssignToVar(fes->collection.get(), varName) ||
               stmtHasIndexAssignToVar(fes->body.get(), varName);
    return false;
}

bool CodeGenerator::doesVarHaveIndexAssign(const std::string& varName) const {
    if (!currentFuncDecl_ || !currentFuncDecl_->body) return true; // conservative
    return stmtHasIndexAssignToVar(currentFuncDecl_->body.get(), varName);
}

// ---------------------------------------------------------------------------

namespace {

// A reference to varName is "OK if it is the array operand of an IndexExpr

struct ReadOnlyUseChecker {
    const std::string& var;
    const CTEngine* ctEngine;
    // True if some reference to `var` was found in a non-read-only position.
    bool violation = false;

    explicit ReadOnlyUseChecker(const std::string& v, const CTEngine* eng)
        : var(v), ctEngine(eng) {}

    bool isVarIdent(const Expression* e) const {
        return e && e->type == ASTNodeType::IDENTIFIER_EXPR &&
               static_cast<const IdentifierExpr*>(e)->name == var;
    }

    // Return true iff we can prove the call does not mutate any of its
    // pointer-typed arguments.
    bool isCallNonMutating(const std::string& callee) const {
        // Built-ins: ask the unified effect table.
        if (BuiltinEffectTable::isPure(callee)) return true;
        if (BuiltinEffectTable::isReadOnly(callee)) return true;
        if (BuiltinEffectTable::isMutating(callee)) return false;
        // I/O builtins (print, etc.) read their args without mutating.
        if (ctEngine && ctEngine->isPure(callee)) return true;
        return false;
    }

    void visitExpr(const Expression* e) {
        if (!e || violation) return;
        switch (e->type) {
        case ASTNodeType::LITERAL_EXPR:
            return;
        case ASTNodeType::IDENTIFIER_EXPR:
            if (static_cast<const IdentifierExpr*>(e)->name == var) {
                // Bare identifier reference outside a recognized read-only
                violation = true;
            }
            return;
        case ASTNodeType::INDEX_EXPR: {
            auto* idx = static_cast<const IndexExpr*>(e);
            // `var[i]` read is always OK; only the index sub-expression
            if (!isVarIdent(idx->array.get())) {
                visitExpr(idx->array.get());
            }
            visitExpr(idx->index.get());
            return;
        }
        case ASTNodeType::INDEX_ASSIGN_EXPR: {
            auto* ia = static_cast<const IndexAssignExpr*>(e);
            // `var[i] = …` is a direct write — violation.
            if (isVarIdent(ia->array.get())) {
                violation = true;
                return;
            }
            visitExpr(ia->array.get());
            visitExpr(ia->index.get());
            visitExpr(ia->value.get());
            return;
        }
        case ASTNodeType::CALL_EXPR: {
            auto* call = static_cast<const CallExpr*>(e);
            const bool nonMut = isCallNonMutating(call->callee);
            for (const auto& arg : call->arguments) {
                if (isVarIdent(arg.get())) {
                    if (!nonMut) {
                        violation = true;
                        return;
                    }
                    // Pass-by-value to a non-mutating callee is OK; no
                    // further inspection needed for this arg.
                    continue;
                }
                visitExpr(arg.get());
            }
            return;
        }
        case ASTNodeType::ASSIGN_EXPR: {
            auto* a = static_cast<const AssignExpr*>(e);
            // Right-hand side mentioning var would create an alias —
            // violation.  Inspect via the bare-identifier rule above.
            visitExpr(a->value.get());
            return;
        }
        case ASTNodeType::BINARY_EXPR: {
            auto* b = static_cast<const BinaryExpr*>(e);
            visitExpr(b->left.get());
            visitExpr(b->right.get());
            return;
        }
        case ASTNodeType::UNARY_EXPR:
            visitExpr(static_cast<const UnaryExpr*>(e)->operand.get());
            return;
        case ASTNodeType::PREFIX_EXPR:
            visitExpr(static_cast<const PrefixExpr*>(e)->operand.get());
            return;
        case ASTNodeType::POSTFIX_EXPR:
            visitExpr(static_cast<const PostfixExpr*>(e)->operand.get());
            return;
        case ASTNodeType::TERNARY_EXPR: {
            auto* t = static_cast<const TernaryExpr*>(e);
            visitExpr(t->condition.get());
            visitExpr(t->thenExpr.get());
            visitExpr(t->elseExpr.get());
            return;
        }
        case ASTNodeType::ARRAY_EXPR: {
            // var appearing inside an array literal would create an alias.
            // Treat as bare reference (handled in IDENTIFIER_EXPR case).
            auto* arr = static_cast<const ArrayExpr*>(e);
            for (const auto& el : arr->elements) visitExpr(el.get());
            return;
        }
        default:
            // Conservative for unknown expression kinds: if `var` appears
            violation = true;
            return;
        }
    }

    void visitStmt(const Statement* s) {
        if (!s || violation) return;
        if (auto* es = dynamic_cast<const ExprStmt*>(s)) {
            visitExpr(es->expression.get());
            return;
        }
        if (auto* vd = dynamic_cast<const VarDecl*>(s)) {
            // A declaration `var x = var` would create an alias.  The
            // bare-IDENTIFIER rule will catch this.
            if (vd->initializer) visitExpr(vd->initializer.get());
            return;
        }
        if (auto* ret = dynamic_cast<const ReturnStmt*>(s)) {
            // Returning the variable (or any expression containing it)
            // creates an unbounded alias — violation.
            if (ret->value) visitExpr(ret->value.get());
            return;
        }
        if (auto* blk = dynamic_cast<const BlockStmt*>(s)) {
            for (const auto& st : blk->statements) visitStmt(st.get());
            return;
        }
        if (auto* ifs = dynamic_cast<const IfStmt*>(s)) {
            visitExpr(ifs->condition.get());
            visitStmt(ifs->thenBranch.get());
            visitStmt(ifs->elseBranch.get());
            return;
        }
        if (auto* ws = dynamic_cast<const WhileStmt*>(s)) {
            visitExpr(ws->condition.get());
            visitStmt(ws->body.get());
            return;
        }
        if (auto* dws = dynamic_cast<const DoWhileStmt*>(s)) {
            visitExpr(dws->condition.get());
            visitStmt(dws->body.get());
            return;
        }
        if (auto* fs = dynamic_cast<const ForStmt*>(s)) {
            visitExpr(fs->start.get());
            visitExpr(fs->end.get());
            if (fs->step) visitExpr(fs->step.get());
            visitStmt(fs->body.get());
            return;
        }
        if (auto* fes = dynamic_cast<const ForEachStmt*>(s)) {
            visitExpr(fes->collection.get());
            visitStmt(fes->body.get());
            return;
        }
        // Unknown statement kind — conservative.
        violation = true;
    }
};

}  // namespace

bool CodeGenerator::doesVarHaveOnlyReadOnlyUses(const std::string& varName) const {
    if (!currentFuncDecl_ || !currentFuncDecl_->body) return false;  // conservative
    ReadOnlyUseChecker checker(varName, ctEngine_.get());
    checker.visitStmt(currentFuncDecl_->body.get());
    return !checker.violation;
}




/// Compare two expressions for equality (only handles literals and identifiers).
static bool exprEqual(const Expression* a, const Expression* b) {
    if (!a || !b) return false;
    if (a->type != b->type) return false;
    if (a->type == ASTNodeType::LITERAL_EXPR) {
        const auto* la = static_cast<const LiteralExpr*>(a);
        const auto* lb = static_cast<const LiteralExpr*>(b);
        if (la->literalType != lb->literalType) return false;
        if (la->literalType == LiteralExpr::LiteralType::INTEGER)
            return la->intValue == lb->intValue;
        if (la->literalType == LiteralExpr::LiteralType::STRING)
            return la->stringValue == lb->stringValue;
        return la->floatValue == lb->floatValue;
    }
    if (a->type == ASTNodeType::IDENTIFIER_EXPR) {
        const auto* ia = static_cast<const IdentifierExpr*>(a);
        const auto* ib = static_cast<const IdentifierExpr*>(b);
        return ia->name == ib->name;
    }
    return false;
}

void CodeGenerator::fuseLoops(BlockStmt* block) {
    // Recursively apply to nested blocks first.
    for (auto& stmt : block->statements) {
        if (auto* blk = dynamic_cast<BlockStmt*>(stmt.get())) {
            fuseLoops(blk);
        } else if (auto* fr = dynamic_cast<ForStmt*>(stmt.get())) {
            if (fr->body) {
                if (auto* innerBlk = dynamic_cast<BlockStmt*>(fr->body.get())) {
                    fuseLoops(innerBlk);
                }
            }
        }
    }

    // Single-pass fusion: walk statement list looking for adjacent ForStmt pairs.
    auto& stmts = block->statements;
    size_t i = 0;
    while (i + 1 < stmts.size()) {
        auto* first  = dynamic_cast<ForStmt*>(stmts[i].get());
        auto* second = dynamic_cast<ForStmt*>(stmts[i + 1].get());

        if (!first || !second) {
            ++i;
            continue;
        }
        // At least one must have @fuse.
        if (!first->loopHints.fuse && !second->loopHints.fuse) {
            ++i;
            continue;
        }
        // Must have identical start and end bounds.
        if (!exprEqual(first->start.get(), second->start.get()) ||
            !exprEqual(first->end.get(),   second->end.get())) {
            ++i;
            continue;
        }

        // Merge: build a combined body.
        // The second iterator is aliased to the first via a VarDecl.
        std::vector<std::unique_ptr<Statement>> combined;

        // Only add the alias if the second loop uses a different iterator name.
        if (second->iteratorVar != first->iteratorVar) {
            auto aliasIdent = std::make_unique<IdentifierExpr>(first->iteratorVar);
            aliasIdent->line = first->line;
            auto aliasDecl = std::make_unique<VarDecl>(
                second->iteratorVar, std::move(aliasIdent), false, "");
            aliasDecl->line = first->line;
            combined.push_back(std::move(aliasDecl));
        }

        // Append first body's statements.
        if (auto* firstBlk = dynamic_cast<BlockStmt*>(first->body.get())) {
            for (auto& s : firstBlk->statements)
                combined.push_back(std::move(s));
        } else if (first->body) {
            combined.push_back(std::move(first->body));
        }

        // Append second body's statements.
        if (auto* secondBlk = dynamic_cast<BlockStmt*>(second->body.get())) {
            for (auto& s : secondBlk->statements)
                combined.push_back(std::move(s));
        } else if (second->body) {
            combined.push_back(std::move(second->body));
        }

        auto fusedBody = std::make_unique<BlockStmt>(std::move(combined));

        // Build the fused ForStmt.
        LoopConfig fusedHints = first->loopHints;
        fusedHints.fuse = false; // already fused
        auto fused = std::make_unique<ForStmt>(
            first->iteratorVar,
            std::move(first->start),
            std::move(first->end),
            std::move(first->step),
            std::move(fusedBody),
            first->iteratorType);
        fused->loopHints = fusedHints;
        fused->line = first->line;
        fused->column = first->column;

        // Replace first with fused, erase second.
        stmts[i] = std::move(fused);
        stmts.erase(stmts.begin() + static_cast<ptrdiff_t>(i + 1));

        optStats_.loopsFused++;
        // Don't advance i — try to fuse again with the next statement.
    }
}

void CodeGenerator::generateGlobals(Program* program) {
    if (!program) return;
    for (const auto& gv : program->globals) {
        if (!gv) continue;
        // Use the original unqualified name as the LLVM symbol.
        // Namespace resolution happens at the parser level; no mangling here.
        const std::string llvmName = gv->name;

        // Determine the LLVM type: use the annotation if present, else i64.
        llvm::Type* ty = gv->typeName.empty()
            ? getDefaultType()
            : resolveAnnotatedType(gv->typeName);

        // Build a constant initializer.
        llvm::Constant* initVal = llvm::Constant::getNullValue(ty);
        if (gv->initializer) {
            if (gv->initializer->type == ASTNodeType::LITERAL_EXPR) {
                auto* lit = static_cast<LiteralExpr*>(gv->initializer.get());
                if (lit->literalType == LiteralExpr::LiteralType::INTEGER) {
                    initVal = llvm::ConstantInt::get(ty, static_cast<uint64_t>(lit->intValue), true);
                } else if (lit->literalType == LiteralExpr::LiteralType::FLOAT) {
                    if (ty->isFloatingPointTy())
                        initVal = llvm::ConstantFP::get(ty, lit->floatValue);
                    else
                        initVal = llvm::ConstantInt::get(ty, static_cast<uint64_t>(lit->floatValue), true);
                }
                // String or other: fall back to null (runtime init unsupported for top-level globals)
            }
            // Non-literal initializers at top level: zero-init the global.
        }

        // If a global with this name already exists (e.g. from a previous import
        // of the same file), skip re-declaration to avoid a duplicate symbol.
        if (module->getGlobalVariable(llvmName)) {
            globalVars_[llvmName] = module->getGlobalVariable(llvmName);
            continue;
        }

        auto* llvmGV = new llvm::GlobalVariable(
            *module,
            ty,
            gv->isConst,                              // isConstant
            llvm::GlobalValue::ExternalLinkage,
            initVal,
            llvmName
        );
        llvmGV->setAlignment(llvm::MaybeAlign(8));
        globalVars_[llvmName] = llvmGV;
    }
}

void CodeGenerator::runRLCPass(Program* program, bool verbose) {
    const RLCStats stats = omscript::runRLCPass(program, verbose);
    optStats_.regionsCoalesced += stats.regionsCoalesced;
}

} // namespace omscript
