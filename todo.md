# OmScript Language Feature TODO

This file tracks high-value language features that are desirable but not fully implemented yet.

## High Priority

- [ ] **Generic functions (true type parameters)**
  - Current state: reserved only.
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

- [ ] **Nested namespaces**
  - Current state: namespaces are flat; nesting is disallowed.
  - Reference: `LANGUAGE_REFERENCE.md` §23.
  - Goal: hierarchical namespacing with predictable resolution/import rules.

## Ergonomics / Diagnostics

- [ ] **Named-argument fallback warning for unresolved declarations**
  - Current state: when declaration metadata is unavailable, labels are ignored and call degrades to positional without warning.
  - Reference: `LANGUAGE_REFERENCE.md` §6.3.1.
  - Goal: emit warning or error to prevent subtle call-site bugs.

- [ ] **Finalize v5 global mutable qualifier design**
  - Current state: qualifier is planned but exact keyword is not selected.
  - Reference: `LANGUAGE_REFERENCE.md` §33.
  - Goal: choose keyword, enforce behavior, and provide migration tooling.

## Long-Term

- [ ] **Structured exception syntax (`try` blocks) if error model evolves**
  - Current state: `try` is reserved; no `try {}` syntax.
  - Reference: `LANGUAGE_REFERENCE.md` §2.4.
  - Goal: only add if it integrates cleanly with existing `throw/catch` and diagnostics model.
