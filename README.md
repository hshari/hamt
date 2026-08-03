# hamt

A generation-tracked hash array mapped trie (HAMT) for C++20.

A `hamt::hamt_map` is a mutable, non-copyable map with transient semantics:
`insert` and `erase` modify the map in place, and structural sharing is
enforced per generation. `fork()` cheaply duplicates a map; both the original
and the copy advance to a fresh generation, so subsequent modifications of
either path-copy only the nodes they share with older versions.

The design follows Phil Bagwell's HAMT as popularized by Clojure's
`PersistentHashMap` (Rich Hickey), whose in-place "transient" mutation model
(with an ownership marker instead of a generation counter) this library
mirrors. `refs/PersistentHashMap.java` is the reference implementation (Eclipse
Public License 1.0, see `refs/epl-v10.html`) used to validate the algorithms;
the C++ code is an original implementation.

The library itself is released under the MIT License (see `LICENSE`).

## Usage

```cpp
#include <hamt/hamt.hpp>

hamt::hamt_map<std::string, int> m;
m.insert("alpha", 1);
m.insert("beta", 2);

auto snap = m.fork();       // cheap snapshot; both maps advance a generation
m.insert("gamma", 3);       // snap is unaffected
snap.contains("gamma");     // false
m.contains("gamma");        // true
```

## API

`hamt::hamt_map<Key, Value, Hash = std::hash<Key>, Equal = std::equal_to<Key>>`

| Member | Description |
| --- | --- |
| `insert(Key, Value)` | Inserts or replaces the key, mutating the map in place. Returns `*this` for chaining. No-op if the content is unchanged. |
| `erase(Key)` | Removes the key, mutating the map in place. Returns `*this`. No-op if the key is absent. |
| `fork()` | Returns a new map sharing the current structure and advances both maps to a fresh generation. O(1). |
| `find(Key)` | Iterator to the entry, or `end()` |
| `contains(Key)`, `count(Key)` | Membership tests |
| `size()`, `empty()` | Number of entries |
| `begin()`, `end()` | Forward iterators over `std::pair<const Key, Value>` |
| `operator==`, `operator!=` | Content equality (requires `Value` to be equality-comparable) |
| `swap` | Constant-time |

Construct from `std::initializer_list` or any pair-valued input range.

The map is non-copyable and move-only. Move transfers ownership; the
moved-from map is left empty.

Requirements: `Key` and `Value` copy-constructible; `Hash` default-constructible,
invocable with `const Key&`, result convertible to `std::uint64_t`; `Equal`
a strict predicate on `const Key&`. Both `Hash` and `Equal` must be stateless
(empty classes): the map calls default-constructed instances of them.

## How generations work

Every node records the generation it was created in; every map carries a
current generation `gen_`. A node whose generation equals the map's is private
to that map and is mutated in place. A node from an older generation is shared
with other maps and is path-copied (cloned at the current generation) before
being modified. Because generation numbers are monotonic along any root-to-leaf
path, an owned node's ancestors are always owned: an in-place mutation never
disturbs a node another map can see.

`fork()` advances both maps, so all previously shared nodes become older than
either map's generation. From then on each map clones before writing, keeping
the two fully independent. Between forks, repeated mutations touch only the
map's private nodes and do no copying at all.

Generations are counted in a `std::uint64_t`, capping a map's lineage at
2^64 forks; in Debug builds `fork()` asserts before the counter could wrap.

## Properties

- Expected O(log32 n) lookup, insert, and erase; bounded by 13 levels
  (12 x 5 bits + 4 bits of the 64-bit hash). Between forks, mutation is
  amortized O(1) — only shared path nodes are ever copied.
- Keys with identical hashes are kept in collision buckets that nest one level
  deeper on split, so behavior degrades gracefully to O(n) even for a
  pathological constant hash.
- Iteration is in ascending order of the mixed hash. The user hash is passed
  through a SplitMix64 finalizer so ordering and distribution do not depend on
  the quality of the user's hash (e.g. `std::hash<size_t>` is the identity).
- Iterators are invalidated by any subsequent mutation of the map they came
  from. Iterators into a forked map remain valid while the maps sharing its
  structure continue to mutate (they path-copy, never touching its nodes).
- A map is single-writer: do not mutate it from two threads at once. Read-only
  operations (`find`, `contains`, iteration) on a map that is not being
  mutated are safe.
- `insert` and `erase` provide the basic exception guarantee: on a failed
  allocation the map stays valid and unchanged, though the pass-by-value
  arguments may already have been moved from. Replacing an existing value
  leaves the old one moved-from if the `Value` move assignment itself throws.
- The empty map has a null root; no node allocation happens until the first
  insert.

## Building and testing

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The test suite covers fork semantics, version chains, collision buckets, node
splits and collapses, the final hash level, custom hash/equality,
non-comparable values, iteration order, iterator stability across forks, move
semantics, and a randomized stress test against `std::unordered_map` with
forked snapshots.
