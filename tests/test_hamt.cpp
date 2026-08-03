#include <hamt/hamt.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <ranges>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

static std::uint64_t mix64(std::uint64_t z) noexcept
{
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

struct identity_hash
{
    std::uint64_t operator()(std::uint64_t k) const noexcept { return k; }
};

using map_t = hamt::hamt_map<std::uint64_t, std::uint64_t, identity_hash>;

static_assert(!std::is_copy_constructible_v<map_t>);
static_assert(!std::is_copy_assignable_v<map_t>);
static_assert(std::is_move_constructible_v<map_t>);
static_assert(std::is_nothrow_move_constructible_v<map_t>);
static_assert(std::is_default_constructible_v<map_t>);
static_assert(std::is_same_v<map_t::value_type, std::pair<const std::uint64_t, std::uint64_t>>);
static_assert(std::forward_iterator<map_t::const_iterator>);
static_assert(std::ranges::input_range<map_t>);

struct stateful_hash
{
    int state = 0;
    std::uint64_t operator()(std::uint64_t k) const noexcept { return k + static_cast<std::uint64_t>(state); }
};

template <typename T, typename = void>
struct map_accepts : std::false_type
{
};

template <typename T>
struct map_accepts<T, std::void_t<hamt::hamt_map<std::uint64_t, std::uint64_t, T>>> : std::true_type
{
};

static_assert(!map_accepts<stateful_hash>::value);
static_assert(map_accepts<identity_hash>::value);

static std::uint64_t unmix64(std::uint64_t z) noexcept
{
    z = (z ^ (z >> 31) ^ (z >> 62)) * 0x319642b2d24d8ec3ull;
    z = (z ^ (z >> 27) ^ (z >> 54)) * 0x96de1b173f119089ull;
    return z ^ (z >> 30) ^ (z >> 60);
}

static void test_empty()
{
    map_t m;
    CHECK(m.empty());
    CHECK(m.size() == 0);
    CHECK(m.begin() == m.end());
    CHECK(m.find(1) == m.end());
    CHECK(!m.contains(1));
    CHECK(m.count(1) == 0);
    m.erase(42);
    CHECK(m.empty());
    CHECK(m.size() == 0);
}

static void test_basic_insert_find()
{
    map_t m;
    for (std::uint64_t i = 0; i < 1000; ++i)
        m.insert(i, i * i);
    CHECK(m.size() == 1000);
    for (std::uint64_t i = 0; i < 1000; ++i) {
        CHECK(m.contains(i));
        const auto it = m.find(i);
        CHECK(it != m.end());
        CHECK(it->first == i);
        CHECK(it->second == i * i);
    }
    CHECK(m.find(1000) == m.end());
    CHECK(!m.contains(1000));
}

static void test_fork()
{
    map_t base;
    base.insert(1, 10).insert(2, 20).insert(3, 30);
    map_t a = base.fork();
    a.insert(4, 40);
    CHECK(a.contains(4));
    CHECK(!base.contains(4));
    CHECK(base.size() == 3);
    CHECK(a.size() == 4);
    map_t b = base.fork();
    b.insert(5, 50);
    CHECK(b.contains(5));
    CHECK(!b.contains(4));
    CHECK(a.contains(4));
    CHECK(!a.contains(5));
    map_t c = base.fork();
    c.erase(2);
    CHECK(!c.contains(2));
    CHECK(base.contains(2));
    CHECK(c.size() == 2);
    map_t chain;
    std::vector<map_t> versions;
    for (std::uint64_t i = 0; i < 64; ++i) {
        chain.insert(i, i);
        versions.push_back(chain.fork());
    }
    for (std::size_t v = 0; v < versions.size(); ++v) {
        CHECK(versions[v].size() == v + 1);
        for (std::uint64_t i = 0; i <= v; ++i)
            CHECK(versions[v].contains(i));
        CHECK(!versions[v].contains(v + 1));
    }
}

static void test_fork_chain()
{
    map_t m;
    m.insert(1, 1).insert(2, 2);
    map_t a = m.fork();
    map_t b = a.fork();
    m.insert(3, 3);
    a.insert(4, 4);
    b.insert(5, 5);
    CHECK(m.size() == 3 && a.size() == 3 && b.size() == 3);
    CHECK(m.contains(3) && !a.contains(3) && !b.contains(3));
    CHECK(a.contains(4) && !m.contains(4) && !b.contains(4));
    CHECK(b.contains(5) && !m.contains(5) && !a.contains(5));
    map_t c = m.fork();
    CHECK(c.size() == 3);
    CHECK(c == m);
    m.erase(3);
    CHECK(c.contains(3));
    CHECK(!m.contains(3));
}

static void test_replace_value()
{
    map_t m;
    m.insert(7, 100).insert(8, 200).insert(7, 300);
    CHECK(m.size() == 2);
    CHECK(m.find(7)->second == 300);
    CHECK(m.find(8)->second == 200);
    m.insert(7, 300);
    CHECK(m.size() == 2);
    CHECK(m.find(7)->second == 300);
}

static void test_erase()
{
    map_t m;
    for (std::uint64_t i = 0; i < 500; ++i)
        m.insert(i, i);
    m.erase(0);
    m.erase(250);
    m.erase(499);
    CHECK(m.size() == 497);
    CHECK(!m.contains(0));
    CHECK(!m.contains(250));
    CHECK(!m.contains(499));
    CHECK(m.contains(1));
    CHECK(m.contains(498));
    m.erase(12345);
    CHECK(m.size() == 497);
    map_t e = m.fork();
    while (!e.empty()) {
        const std::uint64_t k = e.begin()->first;
        e.erase(k);
    }
    CHECK(e.empty());
    CHECK(e.size() == 0);
    CHECK(e.begin() == e.end());
    e.insert(9, 9);
    CHECK(e.size() == 1);
    CHECK(e.contains(9));
}

static void test_iteration_order()
{
    std::mt19937_64 rng(12345);
    map_t m;
    std::unordered_map<std::uint64_t, std::uint64_t> ref;
    for (int i = 0; i < 20000; ++i) {
        const std::uint64_t k = rng();
        m.insert(k, k ^ 0xDEADBEEF);
        ref[k] = k ^ 0xDEADBEEF;
    }
    std::vector<std::uint64_t> keys;
    std::vector<std::uint64_t> prev_hashes;
    for (auto it = m.begin(); it != m.end(); ++it) {
        keys.push_back(it->first);
        CHECK(ref.count(it->first) == 1);
        CHECK(ref.at(it->first) == it->second);
        if (!prev_hashes.empty())
            CHECK(mix64(it->first) > prev_hashes.back());
        prev_hashes.push_back(mix64(it->first));
    }
    CHECK(keys.size() == ref.size());
    CHECK(m.size() == ref.size());
    std::vector<std::uint64_t> keys2;
    for (auto it = m.begin(); it != m.end(); ++it)
        keys2.push_back(it->first);
    CHECK(keys == keys2);
    std::size_t n = 0;
    for (const auto& kv : m) {
        (void)kv;
        ++n;
    }
    CHECK(n == m.size());
}static void test_collision_bucket()
{
    struct zero_hash
    {
        std::uint64_t operator()(std::uint64_t) const noexcept { return 0; }
    };
    hamt::hamt_map<std::uint64_t, std::uint64_t, zero_hash> m;
    for (std::uint64_t i = 0; i < 1000; ++i)
        m.insert(i, i * 2);
    CHECK(m.size() == 1000);
    for (std::uint64_t i = 0; i < 1000; ++i) {
        CHECK(m.contains(i));
        CHECK(m.find(i)->second == i * 2);
    }
    CHECK(!m.contains(1001));
    for (std::uint64_t i = 0; i < 500; i += 2)
        m.erase(i);
    CHECK(m.size() == 750);
    CHECK(!m.contains(0));
    CHECK(!m.contains(2));
    CHECK(m.contains(1));
    CHECK(m.contains(999));
    std::size_t count = 0;
    std::vector<std::uint64_t> seen;
    for (const auto& kv : m) {
        ++count;
        seen.push_back(kv.first);
    }
    CHECK(count == 750);
    std::sort(seen.begin(), seen.end());
    CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
    while (m.size() > 1)
        m.erase(m.begin()->first);
    CHECK(m.size() == 1);
    CHECK(m.begin()->second == 999 * 2);
}

static void test_split()
{
    map_t m;
    for (std::uint64_t k = 0; k <= 31 * 32; k += 32)
        m.insert(k, k);
    CHECK(m.size() == 32);
    for (std::uint64_t k = 0; k <= 31 * 32; k += 32) {
        CHECK(m.contains(k));
        CHECK(m.find(k)->second == k);
    }
    struct mixed_hash
    {
        std::uint64_t operator()(std::uint64_t k) const noexcept
        {
            return k < 500 ? 0 : (k < 1000 ? 32 : 64);
        }
    };
    hamt::hamt_map<std::uint64_t, std::uint64_t, mixed_hash> b;
    for (std::uint64_t k = 0; k < 1500; ++k)
        b.insert(k, k);
    CHECK(b.size() == 1500);
    for (std::uint64_t k = 0; k < 1500; ++k)
        CHECK(b.contains(k));
    for (std::uint64_t k = 0; k < 1500; k += 7)
        b.erase(k);
    CHECK(b.size() == 1500 - 215);
    for (std::uint64_t k = 0; k < 1500; ++k)
        CHECK(b.contains(k) == (k % 7 != 0));
}

static void test_last_level()
{
    const std::uint64_t base = 0xFFFFFFFFFFFFFFF0ull;
    map_t m;
    for (std::uint64_t i = 0; i < 16; ++i) {
        CHECK(unmix64(mix64(i)) == i);
        CHECK(unmix64(mix64(base + i)) == base + i);
        m.insert(unmix64(base + i), i);
    }
    CHECK(m.size() == 16);
    std::uint64_t expect = 0;
    for (const auto& kv : m)
        CHECK(kv.second == expect++);
    CHECK(expect == 16);
    for (unsigned i = 0; i < 16; ++i) {
        CHECK(m.contains(unmix64(base + i)));
        CHECK(m.find(unmix64(base + i))->second == i);
    }
    for (unsigned i = 0; i < 16; i += 2)
        m.erase(unmix64(base + i));
    CHECK(m.size() == 8);
    for (unsigned i = 0; i < 16; ++i)
        CHECK(m.contains(unmix64(base + i)) == (i % 2 == 1));
}

static void test_custom_equal()
{
    struct key
    {
        int group;
        int id;
        bool operator==(const key&) const = default;
    };
    struct key_hash
    {
        std::uint64_t operator()(const key& k) const noexcept
        {
            return static_cast<std::uint64_t>(k.group);
        }
    };
    struct key_equal
    {
        bool operator()(const key& a, const key& b) const noexcept
        {
            return a.group == b.group;
        }
    };
    using m_t = hamt::hamt_map<key, std::string, key_hash, key_equal>;
    m_t m;
    m.insert(key{1, 10}, "a");
    m.insert(key{2, 20}, "b");
    m.insert(key{1, 99}, "a2");
    CHECK(m.size() == 2);
    CHECK(m.find(key{1, 12345})->second == "a2");
    CHECK(m.contains(key{1, 7}));
    m.erase(key{2, -1});
    CHECK(!m.contains(key{2, 42}));
    CHECK(m.size() == 1);
}

static void test_non_comparable_value()
{
    struct payload
    {
        int x;
    };
    using m_t = hamt::hamt_map<std::uint64_t, payload>;
    static_assert(!std::equality_comparable<payload>);
    m_t m;
    m.insert(1, payload{10});
    m.insert(2, payload{20});
    m.insert(1, payload{99});
    CHECK(m.size() == 2);
    CHECK(m.find(1)->second.x == 99);
    CHECK(m.find(2)->second.x == 20);
    m.erase(1);
    CHECK(!m.contains(1));
    CHECK(m.size() == 1);
    std::size_t n = 0;
    for (const auto& kv : m)
        n += (kv.first + static_cast<std::uint64_t>(kv.second.x));
    CHECK(n == 22);
}

static void test_constructors()
{
    map_t m{{1, 1}, {2, 4}, {3, 9}};
    CHECK(m.size() == 3);
    CHECK(m.contains(2));
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> v{{10, 1}, {11, 2}, {10, 5}};
    map_t m2(v.begin(), v.end());
    CHECK(m2.size() == 2);
    CHECK(m2.find(10)->second == 5);
}

static void test_swap()
{
    map_t a;
    a.insert(1, 1).insert(2, 2);
    map_t b;
    b.insert(10, 10);
    swap(a, b);
    CHECK(a.size() == 1 && a.contains(10));
    CHECK(b.size() == 2 && b.contains(1) && b.contains(2));
    a.swap(b);
    CHECK(a.size() == 2 && a.contains(1));
    CHECK(b.size() == 1 && b.contains(10));
}

static void test_equality()
{
    map_t a;
    a.insert(1, 1).insert(2, 2).insert(3, 3);
    map_t b;
    b.insert(3, 3).insert(2, 2).insert(1, 1);
    CHECK(a == b);
    CHECK(!(a != b));
    map_t c = a.fork();
    CHECK(c == a);
    c.insert(1, 42);
    CHECK(c != a);
    c.insert(1, 1);
    CHECK(c == a);
    a.erase(3);
    CHECK(a != b);
    b.erase(3);
    CHECK(a == b);
    map_t empty1;
    map_t empty2;
    CHECK(empty1 == empty2);
}

static void test_iterator_snapshot_stability()
{
    map_t m;
    for (std::uint64_t i = 0; i < 100; ++i)
        m.insert(i, i);
    map_t snap = m.fork();
    const auto it = snap.find(50);
    CHECK(it != snap.end());
    CHECK(it->second == 50);
    m.insert(1000, 1);
    m.erase(5);
    m.insert(1001, 2);
    std::vector<std::uint64_t> collected;
    for (auto i1 = it; i1 != snap.end(); ++i1)
        collected.push_back(i1->first);
    std::vector<std::uint64_t> expected;
    for (std::uint64_t i = 0; i < 100; ++i)
        if (mix64(i) >= mix64(50))
            expected.push_back(i);
    std::sort(expected.begin(), expected.end());
    std::sort(collected.begin(), collected.end());
    CHECK(collected == expected);
    for (const auto k : collected)
        CHECK(k < 100);
    CHECK(m.size() == 101);
    CHECK(!m.contains(5));
    CHECK(m.contains(1000));
    CHECK(snap.size() == 100);
    CHECK(snap.contains(5));
    CHECK(!snap.contains(1000));
}

static void test_move()
{
    map_t m;
    m.insert(1, 1).insert(2, 2);
    map_t n = std::move(m);
    CHECK(n.size() == 2);
    CHECK(n.contains(1));
    CHECK(m.empty());
    CHECK(m.size() == 0);
    m.insert(3, 3);
    CHECK(m.size() == 1);
    CHECK(m.contains(3));
    CHECK(n.size() == 2);
    CHECK(!n.contains(3));
    n = std::move(m);
    CHECK(n.size() == 1);
    CHECK(n.contains(3));
    CHECK(m.empty());
}

static void test_move_generation_isolation()
{
    map_t m;
    for (std::uint64_t i = 0; i < 10; ++i)
        m.insert(i, i);
    map_t n = std::move(m);
    map_t snap = n.fork();
    m.insert(100, 100);
    n.insert(200, 200).insert(0, 42);
    CHECK(snap.size() == 10);
    CHECK(snap.find(0)->second == 0);
    CHECK(!snap.contains(100));
    CHECK(!snap.contains(200));
    CHECK(n.size() == 11);
    CHECK(n.find(0)->second == 42);
    CHECK(m.size() == 1);
    CHECK(m.contains(100));
    CHECK(!m.contains(0));
    map_t a;
    a.insert(1, 1).insert(2, 2);
    map_t b;
    b = std::move(a);
    map_t b_snap = b.fork();
    a.insert(3, 3);
    b.insert(1, 10);
    CHECK(b_snap.size() == 2);
    CHECK(b_snap.find(1)->second == 1);
    CHECK(!b_snap.contains(3));
    CHECK(b.find(1)->second == 10);
    CHECK(a.size() == 1);
    CHECK(a.contains(3));
}

static void test_random_stress()
{
    std::mt19937_64 rng(20260731);
    using ref_t = std::unordered_map<std::uint64_t, std::uint64_t>;
    map_t m;
    ref_t ref;
    std::vector<std::pair<map_t, ref_t>> snapshots;
    const std::uint64_t ops = 200000;
    for (std::uint64_t i = 0; i < ops; ++i) {
        const std::uint64_t k = rng();
        switch (i % 4) {
        case 0:
        case 1: {
            const std::uint64_t v = rng();
            m.insert(k, v);
            ref[k] = v;
            break;
        }
        case 2: {
            m.erase(k);
            ref.erase(k);
            break;
        }
        default: {
            const bool in_map = m.contains(k);
            const bool in_ref = ref.count(k) != 0;
            CHECK(in_map == in_ref);
            if (in_map) {
                CHECK(m.find(k)->second == ref.at(k));
                CHECK(m.count(k) == 1);
            }
            break;
        }
        }
        if (i % 5000 == 4999) {
            CHECK(m.size() == ref.size());
            snapshots.emplace_back(m.fork(), ref);
        }
    }
    CHECK(m.size() == ref.size());
    for (const auto& kv : m) {
        CHECK(ref.count(kv.first) == 1);
        CHECK(ref.at(kv.first) == kv.second);
    }
    std::size_t counted = 0;
    for (const auto& kv : ref) {
        CHECK(m.contains(kv.first));
        ++counted;
    }
    CHECK(counted == m.size());
    for (auto& [snap, snap_ref] : snapshots) {
        CHECK(snap.size() == snap_ref.size());
        for (const auto& kv : snap) {
            CHECK(snap_ref.count(kv.first) == 1);
            CHECK(snap_ref.at(kv.first) == kv.second);
        }
    }
}

static void test_string_keys()
{
    hamt::hamt_map<std::string, int> m;
    m.insert("alpha", 1);
    m.insert("beta", 2);
    m.insert("alpha", 3);
    CHECK(m.size() == 2);
    CHECK(m.find("beta")->second == 2);
    CHECK(m.find("alpha")->second == 3);
    m.erase("alpha");
    CHECK(m.size() == 1);
    CHECK(m.contains("beta"));
    int total = 0;
    for (const auto& kv : m)
        total += kv.second;
    CHECK(total == 2);
}

static void test_vector_value()
{
    using m_t = hamt::hamt_map<int, std::vector<int>>;
    m_t m;
    m.insert(1, {1, 2, 3});
    m.insert(2, {});
    CHECK(m.size() == 2);
    CHECK(m.find(1)->second == std::vector<int>({1, 2, 3}));
    m.erase(1);
    CHECK(m.size() == 1);
    m.insert(1, {9});
    CHECK(m.find(1)->second == std::vector<int>({9}));
}

int main()
{
    test_empty();
    test_basic_insert_find();
    test_fork();
    test_fork_chain();
    test_replace_value();
    test_erase();
    test_iteration_order();
    test_collision_bucket();
    test_split();
    test_last_level();
    test_custom_equal();
    test_non_comparable_value();
    test_constructors();
    test_swap();
    test_equality();
    test_iterator_snapshot_stability();
    test_move();
    test_move_generation_isolation();
    test_random_stress();
    test_string_keys();
    test_vector_value();
    if (g_failures == 0)
        std::printf("All tests passed.\n");
    else
        std::printf("%d check(s) FAILED.\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

