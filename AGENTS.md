# AGENTS.md

## Code & API

* **C/C++:** C11 core, C++ compatible public headers. No unapproved API changes.
* **Types:** Use `bool`. Prefer `uint32_t`/`uint64_t` unless negatives are needed.
* **Style:** `//` comments, Allman braces, 4 spaces, no tabs. Don't reformat unrelated code. Follow existing conventions.
* **Design:** Avoid needless abstraction or macros. Factorize duplicated blocks >50 lines.
* Line length is 160 characters and separators are 127-chars like // ----

## Code Quality

* **Streamlined:** Prefer the simplest correct implementation. Reduce unnecessary LoC, branches, helper layers, and duplicated logic without sacrificing readability, correctness, or maintainability.
* **Clarity:** Do not shorten code through cryptic names, excessive compression, clever tricks, or code obfuscation. Concise code must remain straightforward to understand.
* **Minimalism:** Before adding new code, look for opportunities to reuse existing logic, remove redundant steps, or simplify the design. Prefer eliminating code over adding abstractions to manage it.

## Memory

* **Libraries:** No `malloc`/`free`; prefer caller-provided buffers.
* **Tools:** `malloc`/`free` allowed outside critical hot loops.
* **All:** Use static heap for ≤256KiB; project memory interface for >256KiB. Make ownership explicit; don't retain caller pointers unless required. Avoid hidden allocations or large temporary copies.

## State & Dependencies

* **State:** No mutable global state; use explicit context structs or parameters.
* **Thread:** Single-threaded; no mutexes, atomics, or lock-free structures.
* **Deps:** No unapproved third-party dependencies. Preserve platform and build requirements.
* **Math:** Use project's SplitMix32; never `rand()`.

## Performance

* Favor compact data layouts, cache locality, and linear buffer access.
* Avoid redundant passes over large data.
* Require profiled/measured improvements; no speculative complexity.
* Never sacrifice correctness or maintainability for unmeasured optimization.

## Build & Test

* **Commands:** `cmake -B build -S .` (Config), `cmake --build build` (Build), `rm -rf build/` (Clean), `./build/unit_tests` (Test).
* **Rules:** Every bug fix requires a regression test. Never modify or weaken tests just to pass. Verify tests actually run before claiming success.
* Test behavior and results, not implementation details. Never assume or enforce a particular implementation.
* No harness tests.


## Completion Criteria

* Verify the following before reporting completion: implementation is finished, build succeeds, tests pass, coding rules are followed, and no unrelated code is modified.