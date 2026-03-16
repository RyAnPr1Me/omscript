# E-Graph Optimization & Superoptimizer Architecture

## Overview

OmScript's compiler includes two advanced optimization systems that operate
alongside the standard LLVM optimization pipeline:

1. **E-Graph Equality Saturation** — an AST-level optimizer that discovers
   globally optimal expression representations using equality saturation
2. **Superoptimizer** — an LLVM IR-level optimizer that finds optimal
   instruction sequences through idiom recognition and enumerative synthesis

Together, these catch optimization opportunities that LLVM's standard pass
pipeline misses, producing measurably faster output.

## Compilation Pipeline

```
Source Code
    │
    ▼
[Lexer] → Tokens → [Parser] → AST
    │
    ▼
┌─────────────────────────────────┐
│  E-Graph Equality Saturation    │  ← AST-level (O2+)
│  • Algebraic simplification     │
│  • Constant folding             │
│  • Strength reduction           │
│  • Expression normalization     │
└─────────────────────────────────┘
    │
    ▼
[CodeGenerator] → LLVM IR
    │
    ▼
┌─────────────────────────────────┐
│  LLVM Standard Pipeline         │  ← Module-level (O1+)
│  • mem2reg, GVN, DCE            │
│  • Inlining, IPSCCP             │
│  • Loop vectorization           │
│  • SLP vectorization            │
└─────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────┐
│  Superoptimizer                 │  ← IR-level (O2+)
│  • Idiom recognition            │
│  • Algebraic simplification     │
│  • Branch-to-select             │
│  • Enumerative synthesis        │
└─────────────────────────────────┘
    │
    ▼
Machine Code
```

---

## 1. E-Graph Equality Saturation

### What is an E-Graph?

An **e-graph** (equivalence graph) is a data structure that compactly
represents many equivalent programs simultaneously. Instead of applying
rewrites destructively (replacing the old expression), the e-graph *adds*
the new expression alongside the old one and marks them as equivalent.

After all rewrite rules have been applied to saturation, a **cost model**
extracts the cheapest equivalent expression — guaranteeing a globally
optimal result rather than being trapped in a local optimum.

### Architecture

```
include/egraph.h          → Data structures: ENode, EClass, EGraph, Pattern, CostModel
src/egraph.cpp            → Core implementation: union-find, saturation, extraction
src/egraph_optimizer.cpp  → AST ↔ E-graph bridge
```

### Key Components

#### ENode
A single operation with children referencing e-classes:
```
ENode { op: Add, children: [class_3, class_7] }
ENode { op: Const, value: 42 }
ENode { op: Var, name: "x" }
```

#### EClass
An equivalence class of ENodes that all compute the same value:
```
EClass #5 = { Add(x, 0), x, Add(0, x) }  // All equivalent to x
```

#### Union-Find
Maintains the equivalence relation between classes with O(α(n)) amortized
operations using path compression and union by rank.

#### Pattern Matching
Rewrite rules use a pattern language to match against e-classes:
```
Pattern::OpPat(Op::Add, {Pattern::Wild("x"), Pattern::ConstPat(0)})
```
matches any expression of the form `?x + 0`.

#### Cost Model
Assigns costs to operations based on x86-64 instruction latencies:
| Operation | Cost (cycles) |
|-----------|--------------|
| Const/Var | 0.1          |
| Add/Sub   | 1.0          |
| Shift     | 1.0          |
| Multiply  | 3.0          |
| Divide    | 25.0         |
| Pow       | 50.0         |

### Optimization Rules

The e-graph includes **60+ rewrite rules** organized into three categories:

#### Algebraic Rules
| Rule | Pattern | Replacement |
|------|---------|-------------|
| add_zero | x + 0 | x |
| sub_zero | x - 0 | x |
| sub_self | x - x | 0 |
| mul_one | x * 1 | x |
| mul_zero | x * 0 | 0 |
| div_one | x / 1 | x |
| div_self | x / x | 1 |
| mod_one | x % 1 | 0 |
| double_neg | -(-x) | x |
| mul_two | x * 2 | x + x |
| mul_pow2 | x * 2^n | x << n |
| div_pow2 | x / 2^n | x >> n |
| add_comm | a + b | b + a |
| mul_comm | a * b | b * a |
| add_assoc | (a+b)+c | a+(b+c) |
| distribute | a*(b+c) | a*b+a*c |
| factor | a*b+a*c | a*(b+c) |

#### Comparison Rules
| Rule | Pattern | Replacement |
|------|---------|-------------|
| eq_self | x == x | 1 |
| ne_self | x != x | 0 |
| lt_self | x < x | 0 |
| le_self | x <= x | 1 |
| not_eq | !(a==b) | a != b |
| not_lt | !(a<b) | a >= b |
| sub_eq_zero | (x-y)==0 | x == y |
| double_not | !!x | x |

#### Bitwise Rules
| Rule | Pattern | Replacement |
|------|---------|-------------|
| xor_self | x ^ x | 0 |
| and_self | x & x | x |
| or_self | x \| x | x |
| xor_zero | x ^ 0 | x |
| and_zero | x & 0 | 0 |
| or_zero | x \| 0 | x |
| double_bitnot | ~~x | x |
| shl_zero | x << 0 | x |
| demorgan_and | ~(a&b) | ~a\|~b |
| demorgan_or | ~(a\|b) | ~a&~b |

### Scalability

The e-graph avoids exponential blowup through:
- **Hash-consing**: Deduplicates identical nodes automatically
- **Node limit**: Configurable maximum (default: 10,000 for per-expression, 50,000 for full saturation)
- **Iteration limit**: Maximum saturation iterations (default: 15-30)
- **Early termination**: Stops when no new merges occur (fixpoint)

### Integration

The e-graph optimizer runs at O2+ before LLVM code generation:
```cpp
// In CodeGenerator::generate():
if (enableEGraph_ && optimizationLevel >= OptimizationLevel::O2) {
    egraph::optimizeProgram(program);
}
```

Controlled via: `-fegraph` (default on at O2+) / `-fno-egraph`

---

## 2. Superoptimizer

### What is a Superoptimizer?

A **superoptimizer** finds the shortest or cheapest instruction sequence
that computes the same function as a given sequence. Unlike traditional
peephole optimizers, it searches the space of possible programs rather
than relying solely on hand-written patterns.

### Architecture

```
include/superoptimizer.h  → Interface: config, stats, entry points
src/superoptimizer.cpp    → Implementation: idiom detection, synthesis, algebraic simp
```

### Four Optimization Phases

#### Phase 1: Idiom Recognition
Detects high-level patterns in low-level LLVM IR and replaces them with
optimal intrinsics:

| Idiom | Pattern | Replacement |
|-------|---------|-------------|
| Rotate Left | `(x<<c) \| (x>>(w-c))` | `llvm.fshl(x, x, c)` |
| Rotate Right | `(x>>c) \| (x<<(w-c))` | `llvm.fshr(x, x, c)` |
| Absolute Value | `x<0 ? -x : x` | `llvm.abs(x)` |
| Abs (XOR trick) | `(x^(x>>31))-(x>>31)` | `llvm.abs(x)` |
| Int Min | `select(a<b, a, b)` | `llvm.smin(a, b)` |
| Int Max | `select(a>b, a, b)` | `llvm.smax(a, b)` |
| Power-of-2 Test | `(x&(x-1))==0` | `ctpop(x) <= 1` |
| Bitfield Extract | `(x>>s) & ((1<<w)-1)` | Recognized for BEXTR |

#### Phase 2: Algebraic Simplification
Applies identity simplifications on LLVM IR that instcombine may miss
when instructions span basic blocks or have multiple uses:

- `(x * c1) * c2 → x * (c1*c2)` — constant combination
- `(x + c1) + c2 → x + (c1+c2)` — constant combination
- `(x << c) >> c → x & mask` — zero extension optimization
- `x - x → 0`, `x ^ x → 0` — self-cancellation
- `x & x → x`, `x | x → x` — idempotence

#### Phase 3: Branch Simplification
Converts simple diamond-shaped branch patterns to branchless `select`:

```
Before:                      After:
  if (cond)                    x = select(cond, a, b)
    x = a
  else
    x = b
```

Only applied when:
- Both branches have ≤ 2 instructions
- No side effects in either branch
- Both branches merge to the same block

#### Phase 4: Enumerative Synthesis
For expensive instructions, searches for cheaper equivalent sequences:

| Original | Synthesized | Cost Reduction |
|----------|-------------|----------------|
| `x * 3` | `(x << 1) + x` | 3 → 2 cycles |
| `x * 5` | `(x << 2) + x` | 3 → 2 cycles |
| `x * 7` | `(x << 3) - x` | 3 → 2 cycles |
| `x * 9` | `(x << 3) + x` | 3 → 2 cycles |
| `x * 15` | `(x << 4) - x` | 3 → 2 cycles |
| `x * 17` | `(x << 4) + x` | 3 → 2 cycles |
| `x / 2^n` (unsigned) | `x >> n` | 25 → 1 cycles |
| `x % 2^n` (unsigned) | `x & (2^n-1)` | 25 → 1 cycles |

### Cost Model

The superoptimizer uses a detailed cost model based on x86-64 instruction
latencies:

| Category | Instructions | Cost (cycles) |
|----------|-------------|---------------|
| Free | bitcast, phi | 0.0 |
| 1-cycle ALU | add, sub, and, or, xor, cmp, shift | 1.0 |
| Select | cmov | 1.5 |
| Multiply | imul | 3.0 |
| FP arith | fadd, fsub, fmul | 4.0 |
| Memory | load, store | 4.0 |
| Intrinsics | ctpop, ctlz, bswap, fshl | 1.0 |
| FP divide | fdiv | 15.0 |
| Int divide | idiv, udiv | 25.0 |

### Integration

The superoptimizer runs after LLVM's standard pipeline at O2+:
```cpp
// In CodeGenerator::runOptimizationPasses():
if (enableSuperopt_ && optimizationLevel >= OptimizationLevel::O2) {
    superopt::superoptimizeModule(*module, config);
}
```

At O3, synthesis parameters are more aggressive:
- `maxInstructions = 5` (vs 3 at O2)
- `costThreshold = 0.9` (vs 0.8 at O2)

Controlled via: `-fsuperopt` (default on at O2+) / `-fno-superopt`

---

## 3. Benchmarking

Run the benchmark suite:
```bash
bash run_egraph_benchmark.sh          # Full benchmarks
bash run_egraph_benchmark.sh --quick  # Quick mode (1 run each)
```

Benchmark programs:
- **Fibonacci** — recursive call overhead
- **Sum of squares** — loop + arithmetic optimization
- **Constant folding** — e-graph constant propagation
- **Bitwise popcount** — bitwise operation optimization
- **Strength reduction** — multiply-to-shift optimization
- **Algebraic identity** — identity elimination

The benchmark compares:
- O0 (no optimization, baseline)
- O2 (standard + e-graph + superopt)
- O3 (aggressive + e-graph + superopt)
- C compiled with gcc -O3

---

## 4. Testing

### Unit Tests

```bash
cd build && ctest --output-on-failure
```

| Test Suite | Tests | Coverage |
|-----------|-------|----------|
| egraph_test | 62 | E-graph data structure, pattern matching, all rule categories, saturation, extraction, AST round-trip |
| superoptimizer_test | 32 | Cost model, idiom detection, idiom replacement, algebraic simplification, synthesis, concrete evaluation, integration |

### Key Test Categories

**E-Graph Tests:**
- Data structure operations (add, find, merge, hash-consing)
- Pattern matching (wildcards, constants, nested patterns)
- Algebraic rules (add-zero, mul-one, sub-self, double negation, etc.)
- Strength reduction (mul→shift, div→shift)
- Constant folding (integer and float)
- Comparison rules (self-equality, complement)
- Bitwise rules (xor-self, De Morgan's laws)
- Cost-based extraction (prefer cheaper alternatives)
- Scalability (node/iteration limits)
- AST round-trip (semantic preservation)
- Integration with LLVM codegen

**Superoptimizer Tests:**
- Cost model accuracy (shifts < mul < div)
- Idiom detection (rotate, abs, min/max, power-of-2, bitfield extract)
- Idiom replacement (emit intrinsics, verify IR)
- Algebraic simplification (x-x, x^x, constant combining)
- Synthesis (mul→shift+add, udiv→shift, urem→and)
- Concrete evaluator (add, mul, xor)
- Module-level optimization
- Integration with full compiler pipeline

---

## 5. Design Decisions

### Why E-Graph at the AST Level?
Operating on the AST before LLVM codegen allows the e-graph to discover
algebraic identities that would be obscured by LLVM's lowering (e.g.,
the alloca/load/store patterns for local variables). After mem2reg promotes
these to SSA, some cross-expression optimizations become harder to spot.

### Why Superoptimizer After LLVM?
The superoptimizer operates on the fully optimized LLVM IR, catching
patterns that LLVM's pass pipeline left behind. Idiom recognition in
particular benefits from seeing the canonical form that instcombine and
GVN produce — making pattern matching reliable and simple.

### Why Not Use `egg` (Rust)?
The compiler is C++17 with LLVM integration. Introducing a Rust dependency
would complicate the build system. Our C++ e-graph implementation is
tailored to OmScript's AST and integrates seamlessly with the existing
codebase. It supports the same core operations (union-find with path
compression, hash-consing, pattern matching, cost-based extraction).

### Avoiding E-Graph Explosion
Commutativity and associativity rules can cause exponential growth. We
mitigate this through:
1. Hard node limit (default 10,000 per expression)
2. Iteration limit (default 15)
3. Hash-consing to deduplicate equivalent nodes
4. Early termination at fixpoint (no new merges)

### Correctness Guarantees
- All rewrite rules are mathematically sound
- IEEE-754 semantics preserved for floats (no `x*0→0`)
- Side-effectful operations (calls, stores) not optimized away
- Superoptimizer verifies IR after each transformation
- 94 unit tests validate all optimization rules and transformations
- All 299 existing integration tests continue to pass
