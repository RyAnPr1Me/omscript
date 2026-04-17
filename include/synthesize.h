#pragma once
#ifndef SYNTHESIZE_H
#define SYNTHESIZE_H

/// @file synthesize.h
/// @brief std::synthesize — OmScript compile-time program synthesis engine.
///
/// std::synthesize is a standard-library function evaluated at compile time
/// by the CF-CTRE machinery.  It enumerates candidate integer expressions over
/// the supplied input variables, verifies them against I/O examples using a
/// fast interpreter, scores them with a cost model, and returns the best one.
///
/// USAGE (OmScript):
///
///   // Synthesize a function body — place std__synthesize as the sole return:
///   fn multiply_add(a: int, b: int, c: int) -> int {
///       return std::synthesize(
///           [[1,2,3,5], [2,3,4,10], [0,5,1,1]]);   // [a,b,c,expected]
///   }
///   // Compiler detects the pattern at compile time, runs synthesis,
///   // and replaces the body with: return a * b + c;
///
///   // Also usable inside comptime blocks:
///   const F = comptime {
///       std::synthesize([[1,1,2],[2,3,5],[3,5,8]]);   // → 3 (fib(4))
///   };
///
/// CALL CONVENTION (stdlib function, called as std::synthesize or std__synthesize):
///
///   std::synthesize(examples)                    — default ops, depth 4
///   std::synthesize(examples, ops)               — explicit ops array
///   std::synthesize(examples, ops, max_depth)    — explicit ops + depth
///   std::synthesize(examples, ops, max_depth, cost_hint)  — + "size"|"speed"
///
/// ARGUMENTS:
///   examples   : int[][]  — each inner array is [in0, in1, …, inN-1, expected]
///   ops        : string[] — operator names to use, e.g. ["+","-","*","/","%",
///                           "&","|","^","<<",">>","**","min","max","abs","neg"]
///                           (default: all integer ops)
///   max_depth  : int      — maximum expression-tree depth (default: 4, max: 8)
///   cost_hint  : string   — "size" or "speed" (default: "speed")
///
/// RETURN VALUE (runtime):
///   Integer result of the synthesized expression applied to the FIRST example's
///   input arguments.  When all inputs in all examples are compile-time constants,
///   the entire call is folded to the synthesized result constant.
///
/// COMPILE-TIME BODY REPLACEMENT:
///   When std::synthesize is the sole expression in a pure function's return
///   statement, the runSynthesisPass() pre-codegen pass replaces the function
///   body with the synthesized AST expression.  The synthesized body then passes
///   through the full optimization pipeline (CF-CTRE → e-graph → superoptimizer).
///
/// IMPLEMENTATION:
///   - Enumerator: BFS over expression trees, bottom-up canonical form
///   - Evaluator:  direct recursive eval over int64_t (no allocation)
///   - Verifier:   all examples must agree; counter-example terminates search
///   - Scorer:     (instruction_count + depth * 0.2) × cost_hint_factor
///   - Budget:     up to 200,000 candidates evaluated (compile-time bounded)

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace omscript {

// Forward declarations.
class Expression;
class FunctionDecl;
class Program;

// ─── SynthExample ─────────────────────────────────────────────────────────────
/// A single verified input/output pair.
/// `inputs[i]` corresponds to parameter `i` of the target function.
/// The last element of the flattened array format (from OmScript int[][]) is
/// the expected output; all preceding elements are inputs.
struct SynthExample {
    std::vector<int64_t> inputs;   ///< Parameter values (N elements)
    int64_t              output;   ///< Expected return value
};

// ─── SynthNode — expression tree node ────────────────────────────────────────

/// Operations available to the synthesizer.
enum class SynthOp : uint8_t {
    // Terminals
    PARAM,      ///< Reference to parameter index (leaf)
    CONST,      ///< Small integer constant (leaf)
    // Unary
    NEG,        ///< -x
    ABS,        ///< abs(x)
    NOT,        ///< ~x (bitwise NOT)
    // Binary
    ADD,        ///< x + y
    SUB,        ///< x - y
    MUL,        ///< x * y
    DIV,        ///< x / y  (safe: returns 0 for y==0)
    MOD,        ///< x % y  (safe: returns 0 for y==0)
    AND,        ///< x & y
    OR,         ///< x | y
    XOR,        ///< x ^ y
    SHL,        ///< x << y  (y clamped to [0,62])
    SHR,        ///< x >> y  (arithmetic, y clamped to [0,62])
    POW,        ///< x ** y  (y clamped to [0,30] to avoid overflow)
    MIN2,       ///< min(x, y)
    MAX2,       ///< max(x, y)
};

/// Compact expression tree node.
/// Leaf nodes (PARAM, CONST) have left==right==nullptr.
/// Unary nodes have right==nullptr.
struct SynthNode {
    SynthOp  op;
    int32_t  value;   ///< PARAM: parameter index; CONST: constant value
    std::unique_ptr<SynthNode> left;
    std::unique_ptr<SynthNode> right;

    SynthNode() : op(SynthOp::CONST), value(0) {}
    explicit SynthNode(SynthOp o, int32_t v = 0) : op(o), value(v) {}
};

// ─── SynthResult ─────────────────────────────────────────────────────────────
struct SynthResult {
    std::unique_ptr<SynthNode> expr;   ///< Synthesized expression tree
    double                     cost;   ///< Lower is better
    int                        depth;  ///< Tree depth
    int                        nodes;  ///< Node count
    int64_t                    firstOutput; ///< Result on first example's inputs
};

// ─── SynthConfig ─────────────────────────────────────────────────────────────
struct SynthConfig {
    std::vector<std::string> ops;     ///< Allowed op names (empty = all)
    int    maxDepth{4};               ///< Maximum expression-tree depth [1..8]
    int    maxCandidates{200000};     ///< Evaluation budget
    bool   preferSize{false};         ///< true = optimise for code size
};

// ─── SynthesisEngine ─────────────────────────────────────────────────────────
/// Stateless synthesis engine: given examples + config, finds the best
/// expression that maps inputs → output for every example.
class SynthesisEngine {
public:
    SynthesisEngine() = default;

    /// Primary entry point.
    /// @param nParams    Number of input parameters.
    /// @param examples   Non-empty set of I/O examples.
    /// @param cfg        Search configuration.
    /// @returns The best matching SynthResult, or nullopt if none found.
    std::optional<SynthResult> synthesize(
        int                              nParams,
        const std::vector<SynthExample>& examples,
        const SynthConfig&               cfg = {}) const;

    /// Evaluate a SynthNode on a specific set of inputs.
    static int64_t eval(const SynthNode* node,
                        const std::vector<int64_t>& inputs);

    /// Render a SynthNode as an OmScript expression string
    /// (parameter names are p0, p1, … unless `paramNames` is provided).
    static std::string render(const SynthNode* node,
                              const std::vector<std::string>& paramNames = {});

    /// Render a SynthNode as a unique OmScript Expression AST subtree.
    static std::unique_ptr<Expression> lowerToAST(
        const SynthNode*                node,
        const std::vector<std::string>& paramNames);

private:
    // Enumerate candidate nodes at a given depth, append to `out`.
    static void enumerate(
        int                              depth,
        int                              nParams,
        const std::vector<SynthOp>&      allowedOps,
        std::vector<std::unique_ptr<SynthNode>>& out);

    static std::vector<SynthOp> resolveOps(const SynthConfig& cfg);
    static double nodeCost(const SynthNode* node, bool preferSize);
    static int    nodeCount(const SynthNode* node);
    static int    nodeDepth(const SynthNode* node);
    static bool   verifyAll(const SynthNode* node,
                            const std::vector<SynthExample>& examples);
};

// ─── Whole-program synthesis pass ────────────────────────────────────────────

/// Run a pre-codegen synthesis pass over `program`.
/// For every pure function whose body consists solely of:
///   return std__synthesize(examples_literal[, ops_literal[, depth_literal]])
/// the pass:
///   1. Extracts the examples from the literal array.
///   2. Runs SynthesisEngine::synthesize().
///   3. Replaces the function body with the synthesized expression.
///   4. Marks the function @pure and @const_eval so downstream passes see it.
///
/// Functions where synthesis fails (no candidate found) are left unchanged
/// (a compile-time warning is emitted).
///
/// @param program   The parsed program (mutated in place).
/// @param verbose   If true, prints synthesis stats to stdout.
void runSynthesisPass(Program* program, bool verbose = false);

} // namespace omscript
#endif // SYNTHESIZE_H
