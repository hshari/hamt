# AGENTS.md

## Project

`hamt` — a generation-tracked hash array mapped trie (HAMT) map for C++20.
Header-only, no external dependencies. Single-writer, mutable, non-copyable,
move-only.

Design model (do not break these invariants):

- `hamt::hamt_map` mutates in place. `insert`/`erase` return `*this` for
  chaining. `fork()` duplicates the map cheaply and advances the generation of
  both itself and the copy.
- Every node records the generation it was created in; the map carries `gen_`.
  A node with `node.gen == map.gen_` is private to that map and may be mutated
  in place. Older nodes are shared with other maps and must be cloned
  (path-copied) before modification.
- Node generations are monotonic along any root-to-leaf path, which guarantees
  an owned node's ancestors are owned; in-place mutations therefore never
  disturb nodes visible to other maps.
- The empty map is a null root. No node allocation before the first insert.
- Iteration is in ascending order of the mixed hash. The user hash is passed
  through a SplitMix64 finalizer (`detail::mix64`).
- Keys with identical hashes live in collision buckets that nest one level
  deeper on split.

## Layout

- `include/hamt/hamt.hpp` — the entire library
- `tests/test_hamt.cpp` — unit tests (plain `CHECK(expr)` macro, no framework)
- `CMakeLists.txt` — INTERFACE library target `hamt` + `hamt_tests` executable
- `PersistentHashMap.java` — Clojure reference implementation (EPL-1.0),
  reference only, not part of the library
- `README.md` — API and design documentation

## Build and test

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Verify all three configurations before finishing:

```sh
cmake --build build --config Debug && ctest --test-dir build -C Debug
cmake -S . -B build-clang -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release
cmake --build build-clang && ctest --test-dir build-clang
```

- MSVC: `/W4 /permissive-`; clang-cl: `-Wall -Wextra -Wpedantic`. Warnings
  are errors in review; build must be warning-free.
- Tests must pass in both Release and Debug (asserts are active in Debug and
  guard structural invariants in `ins`/`del`).

## Conventions

- C++20, constraints via `requires` on the class template.
- Match existing style: no code comments, 4-space indent, braces on new lines.
- New tests go in `tests/test_hamt.cpp`; register them in `main()`. The
  `CHECK` macro increments a global failure counter; the program exits nonzero
  on failure.
- Keys/values are copy-constructible; `Hash` must be default-constructible,
  invocable with `const Key&`, and its result convertible to `std::uint64_t`.
- Iterators are invalidated by mutations of the map they came from; iterators
  into a forked map stay valid while other maps mutate.
- `PersistentHashMap.java` must keep its EPL license header; never modify it.
