# OmScript Codebase Audit — Maintainer Review

This is a full-repo audit, not a feel-good writeup. The code compiles and passes tests, but there are structural decisions here that will hurt the project once it grows past “single maintainer with context loaded in RAM.”

## 1. Architectural Assessment

- **Module boundaries are acceptable, abstraction boundaries are not.**
  Directory split (`src/`, `runtime/`, `include/`) is sane. Internal layering is still leaky.
- **`CodeGenerator` is a god object.**
  `src/codegen.cpp` owns frontend semantics, LLVM lowering, runtime stubs, builtins, object emission, and bytecode generation fallback. That is several subsystems stuffed into one class and one translation unit.
- **Dual backend strategy is half-realized.**
  You have LLVM-native compilation *and* bytecode/JIT (`runtime/vm.cpp`, bytecode sections in `src/codegen.cpp`), but feature parity is incomplete. Example: `Array index assignment is not supported in bytecode mode` and `For-each loops are not supported in bytecode mode` (`src/codegen.cpp:2965`, `src/codegen.cpp:3180`). That is architectural debt disguised as optionality.
- **Dependency direction is mostly correct, cohesion is mixed.**
  Parser → AST → Codegen is expected. But codegen also embeds runtime-facing concerns (printing, traps, libc calls), so every semantic tweak risks collateral damage in backend/runtime glue.
- **Hidden complexity exists in scope/state handling.**
  The scope stack / binding restoration is manual and fragile, with explicit mismatch checks (`Scope tracking mismatch in codegen`, `src/codegen.cpp:532`). If your architecture needs runtime assertions to prove its own bookkeeping, the bookkeeping is too implicit.

## 2. Code Quality

- **Naming is mostly readable.**
  This is one of the few consistently good parts.
- **Function/class responsibility is overloaded.**
  `src/codegen.cpp` is huge and mixes concerns. This is not “efficient,” it is a maintainability trap.
- **Too much switch-and-cast plumbing.**
  Repeated `dynamic_cast` and node-type switching in parser/codegen (`src/parser.cpp:433`, `480`, `766`) signals missing AST visitor infrastructure.
- **Error handling style is repetitive string throwing.**
  `throw std::runtime_error(...)` is everywhere. Works for toy tooling, scales badly for structured diagnostics, IDE integration, or machine-readable output.
- **Legacy/prototype patterns survived into core code.**
  Manual tagged-union lifecycle in `runtime/value.h` with placement-new/destructor calls is correct *today*, but one careless edit turns it into UB tomorrow.

## 3. Correctness & Safety

- **Manual union lifetime management is high-risk.**
  `Value` stores `RefCountedString` in a union (`runtime/value.h:194-198`) with explicit constructor/destructor dispatch. This is legal but brittle; one missed branch in copy/move/assignment is memory corruption territory.
- **Parser performs semantic rewrites without a dedicated validation phase.**
  Assignment/postfix restrictions are enforced ad hoc inside parse routines (`src/parser.cpp:430+`, `729+`). This mixes syntax and semantic legality checks in ways that are easy to regress.
- **Bytecode safety checks are good.**
  VM bounds checks (`ensureReadable`) and jump target validation reduce crash risk. This is one area where paranoia is correctly applied.
- **Error recovery exists but is shallow.**
  `synchronize()` exists (`src/parser.cpp:60`) and multi-error aggregation is an improvement, but there is no typed diagnostic model, so downstream tooling is handicapped.

## 4. Performance Analysis

- **Real bottlenecks are likely in runtime string handling and repeated runtime calls.**
  String concatenation in codegen lowers to `strlen + malloc + strcpy + strcat` (`src/codegen.cpp:857-889`) for each operation. That is allocation-heavy and cache-hostile in string-heavy programs.
- **No obvious catastrophic algorithmic mistakes in compiler frontend.**
  Recursive descent and AST construction are straightforward.
- **Potential unnecessary overhead from repeated dynamic dispatch patterns.**
  Frequent type checks/casts in hot compiler paths are acceptable at current scale, but they are not free.
- **Premature optimization is limited; under-instrumentation is the issue.**
  You have optimization levels (`-O0..-Ofast`) but no profiling evidence in-tree proving where time is actually spent. This is tuning by habit, not data.

## 5. Maintainability

- **Hard to modify safely once features expand.**
  Large centralized codegen logic means low local reasoning. Small features will require edits in several fragile switch blocks.
- **Debuggability is mixed.**
  Human-readable runtime errors exist, but structured diagnostics do not.
- **Test coverage is broad but not a silver bullet.**
  The suite is large and catches many regressions, but tests do not fix architecture entropy.
- **Documentation is partly aspirational.**
  Some docs present the project as cleaner than it is under the hood; the backend split and feature parity gaps are non-trivial.

## 6. Engineering Smells

- **Needless abstraction gap:** dual execution backends without full semantic parity.
- **Cargo-cult “support everything” behavior:** linking against a wide LLVM component set in CMake increases build complexity for unclear gain.
- **Smart code reducing clarity:** manual union lifetime management and explicit reference counting where safer standard facilities could reduce risk.
- **Framework/library abuse:** not severe, but codegen directly wiring libc/runtime calls everywhere makes the backend harder to evolve.
- **Reinvented wheel:** custom ref-counted string (`runtime/refcounted.h`) for a project that is otherwise not allocation-constrained enough to justify this maintenance burden.

## 7. Security Concerns

- **Input validation:** reasonable in lexer/parser/VM bounds handling.
- **Trust boundaries:** compiler invoking linker uses `llvm::sys::ExecuteAndWait` with separated args (`src/compiler.cpp:142`), avoiding shell injection. Good.
- **Data exposure:** no obvious secret handling issues in repo code.
- **Dependency/build risk:** heavy LLVM linkage surface increases supply-chain/build reproducibility complexity.
- **Denial-of-service vectors:** parser/runtime can still be stressed with huge inputs; there is no explicit global compile-time resource budgeting beyond VM stack/depth limits.

## 8. Top 10 Critical Problems (ranked by production risk)

1. **Monolithic codegen architecture** (`src/codegen.cpp`) — high regression risk for any feature work.
2. **Backend feature mismatch** (LLVM vs bytecode unsupported constructs) — inconsistent behavior paths.
3. **Manual tagged-union lifetime management in `Value`** — correctness is fragile under maintenance churn.
4. **Hand-rolled non-atomic refcount string type** (`runtime/refcounted.h`) — unsafe for concurrency evolution.
5. **Ad hoc semantic checks in parser** — syntax/semantics boundary is blurry and brittle.
6. **String concatenation lowering is allocation-heavy** — performance cliff for realistic string workloads.
7. **Error model is plain exceptions only** — poor tooling interoperability and weak diagnostics pipeline.
8. **Scope restoration complexity in codegen** — explicit mismatch guards indicate fragile invariants.
9. **Over-linked LLVM component set** — increased build/link fragility and unnecessary footprint.
10. **No resource-budget controls for compile-time input abuse** — potential CPU/memory exhaustion scenarios.

## 9. What Must Be Rewritten

- **`CodeGenerator` decomposition is mandatory, not optional.**
  Split IR lowering, builtin/runtime symbol wiring, object emission, and bytecode emission into separate units with explicit interfaces.
- **`Value` representation should be redesigned.**
  Replace manual union lifecycle with a safer tagged representation (`std::variant`-style approach or equivalent) unless profiling proves this is a hard blocker.
- **Backend strategy needs a decision.**
  Either commit to parity between LLVM and bytecode paths or remove one path. Shipping two inconsistent semantics engines is worse than shipping one solid engine.
- **Diagnostics pipeline should be rebuilt around structured errors.**
  Keep human-readable messages, add typed diagnostics for tooling and deterministic testing.

## 10. Final Maintainer Verdict

- **Would I merge this as-is?**
  For a hobby compiler milestone: yes. For “production tomorrow” with expected growth: no.
- **Would I block release?**
  I would block any release that claims stable backend parity or long-term maintainability without addressing the architecture split and codegen monolith.
- **What level of engineer likely wrote this?**
  Competent mid-level to senior-leaning implementer on language mechanics, weaker on long-horizon architecture discipline.
- **Long-term survivability rating (1–10):** **5/10**.
  It works now, but the current structure will tax every future change until major refactoring happens.
