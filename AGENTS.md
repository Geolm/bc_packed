# AGENTS.md

## Code & API

- C11 core; public headers C++ compatible. No unapproved API changes.
- Use `bool`; prefer `uint32_t`/`uint64_t` unless negatives are needed.
- Style: `//` comments, Allman braces, 4 spaces, no tabs. Preserve existing conventions; don't reformat unrelated code.
- Avoid needless abstractions/macros. Factor duplicated blocks >50 lines.
- Line length: 160 chars. Separators: 127 chars (`// ----`).

## Code Quality

- Prefer the simplest correct implementation: minimize LoC, branches, helpers, and duplication without hurting readability, correctness, or maintainability.
- Keep concise code clear: no cryptic names, excessive compression, clever tricks, or obfuscation.
- Before adding code, reuse existing logic, remove redundancy, or simplify the design. Prefer deleting code over adding abstractions.

## Memory

- Libraries: no `malloc`/`free`; prefer caller-provided buffers.
- Tools: `malloc`/`free` allowed outside critical hot loops.
- Static heap for ≤256 KiB; project memory interface for >256 KiB.
- Make ownership explicit. Don't retain caller pointers unless required. Avoid hidden allocations and large temporary copies.

## State & Dependencies

- No mutable global state; use explicit contexts/parameters.
- Single-threaded: no mutexes, atomics, or lock-free structures.
- No unapproved third-party dependencies. Preserve platform/build requirements.
- Use RNG SplitMix32; never `rand()`.

## Performance

- Favor compact layouts, cache locality, and linear buffer access.
- Avoid redundant passes over large data.
- Optimize only based on measured/profiled improvements; no speculative complexity.
- Never sacrifice correctness or maintainability for unmeasured optimization.

## Build & Test

- Config: `cmake -B build -S . -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Clean: `rm -rf build/`
- Test: `./build/unit_tests`
- Every bug fix requires a regression test.
- Never modify/weaken tests to make them pass.
- Verify tests actually run before claiming success.
- Test behavior/results, not implementation details; don't enforce a specific implementation.
- No harness tests.

## Completion

Before reporting completion, verify: implementation finished, build succeeds, tests pass, rules followed, and no unrelated code changed.