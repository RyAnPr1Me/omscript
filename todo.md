# OmScript Language Feature TODO

This file tracks high-value language features that are desirable but not fully implemented yet.

## High Priority

- [ ] **Generic functions (true type parameters)**
  - Current state: `fn name<T>(...)` declaration syntax is parsed and a warning is emitted; type parameters are not checked or used in codegen.
  - Reference: `LANGUAGE_REFERENCE.md` §6.5.
  - Goal: accept `fn name<T>(...) -> T` and perform real type-checked specialization/inference.

- [ ] **Real `@repr(soa)` lowering**
  - Current state: syntax is parsed but has no codegen effect.
  - Reference: `LANGUAGE_REFERENCE.md` §4.4.6 / struct repr table.
  - Goal: transform struct-of-arrays layout and document ABI/optimization behavior.

- [ ] **Proper enum payload variants + richer `when` matching**
  - Current state: enum constants work; payload-style variant matching is reserved.
  - Reference: `LANGUAGE_REFERENCE.md` §15.3.
  - Goal: algebraic-data-type style variants with destructuring match arms.

## Medium Priority

- [ ] **First-class reference types outside borrow declarations**
  - Current state: borrow-variable references work; refs in params/returns/fields are not formalized.
  - Reference: `LANGUAGE_REFERENCE.md` §4.4.3.
  - Goal: stable `&T` / `&mut T` typing in signatures and aggregate fields.

- [ ] **True closures (lambda capture support)**
  - Current state: lambdas do not capture outer variables.
  - Reference: `LANGUAGE_REFERENCE.md` §6.11.
  - Goal: lexical captures with clear ownership/borrow rules.

- [x] **Nested namespaces**
  - Current state: ~~namespaces are flat; nesting is disallowed~~ **Fully implemented**: `namespace A { namespace B { ... } }` nesting to arbitrary depth now supported.
  - Reference: `LANGUAGE_REFERENCE.md` §23.8.
  - Goal: hierarchical namespacing with predictable resolution/import rules.

## Ergonomics / Diagnostics

- [x] **Named-argument fallback warning for unresolved declarations**
  - Current state: parser now emits a warning when named arguments are used and declaration metadata is unavailable; call still degrades to positional order.
  - Reference: `LANGUAGE_REFERENCE.md` §6.3.1.
  - Follow-up: consider promoting this to an error under stricter language modes.

- [x] **Finalize v5 global mutable qualifier design**
  - Current state: keyword `mut` chosen and accepted (`global mut var`). Old `global var` still compiles without warning in v4.x. Deprecation warning for bare `global var` will be enforced in v5.0.
  - Reference: `LANGUAGE_REFERENCE.md` §33.
  - Follow-up: add deprecation warning under a `--Werror=mutable-global` flag or v5.0 major bump.

## Long-Term

- [ ] **Structured exception syntax (`try` blocks) if error model evolves**
  - Current state: `try` is reserved; no `try {}` syntax.
  - Reference: `LANGUAGE_REFERENCE.md` §2.4.
  - Goal: only add if it integrates cleanly with existing `throw/catch` and diagnostics model.
