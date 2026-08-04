#include <hamt/hamt.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

std::uint64_t mix64(std::uint64_t z) noexcept
{
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

std::uint64_t fnv1a(const char* data, std::size_t len) noexcept
{
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= 0x100000001b3ull;
    }
    return h;
}

struct int_hash
{
    std::uint64_t operator()(std::uint64_t k) const noexcept { return mix64(k); }
};

struct string_hash
{
    std::uint64_t operator()(const std::string& s) const noexcept
    {
        return fnv1a(s.data(), s.size());
    }
};

struct pcg32
{
    std::uint64_t state = 0x853c49e6748fea9bull;
    std::uint64_t inc = 0xda3e39cb94b95bdbull;

    std::uint32_t next() noexcept
    {
        const std::uint64_t old = state;
        state = old * 6364136223846793005ull + inc;
        const std::uint32_t xorshifted =
            static_cast<std::uint32_t>(((old >> 18) ^ old) >> 27);
        const int rot = static_cast<int>(old >> 59);
        return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
    }

    std::uint64_t next64() noexcept
    {
        return (static_cast<std::uint64_t>(next()) << 32) | next();
    }

    std::uint64_t bounded(std::uint64_t bound) noexcept
    {
        if (bound == 0)
            return 0;
        return next64() % bound;
    }
};

template <typename Map>
struct u64_ops;

template <>
struct u64_ops<hamt::hamt_map<std::uint64_t, std::uint64_t, int_hash>>
{
    using map = hamt::hamt_map<std::uint64_t, std::uint64_t, int_hash>;

    static void put(map& m, std::uint64_t k, std::uint64_t v) { m.insert(k, v); }
    static void erase(map& m, std::uint64_t k) { m.erase(k); }
    static const std::uint64_t* get(const map& m, std::uint64_t k)
    {
        auto it = m.find(k);
        return it == m.end() ? nullptr : &it->second;
    }
    static map duplicate(map& m) { return m.fork(); }
};

template <>
struct u64_ops<std::unordered_map<std::uint64_t, std::uint64_t, int_hash>>
{
    using map = std::unordered_map<std::uint64_t, std::uint64_t, int_hash>;

    static void put(map& m, std::uint64_t k, std::uint64_t v)
    {
        m.insert_or_assign(k, v);
    }
    static void erase(map& m, std::uint64_t k) { m.erase(k); }
    static const std::uint64_t* get(const map& m, std::uint64_t k)
    {
        auto it = m.find(k);
        return it == m.end() ? nullptr : &it->second;
    }
    static map duplicate(const map& m) { return m; }
};

template <typename Map>
struct str_ops;

template <>
struct str_ops<hamt::hamt_map<std::string, std::string, string_hash>>
{
    using map = hamt::hamt_map<std::string, std::string, string_hash>;

    static void put(map& m, const std::string& k, const std::string& v)
    {
        m.insert(k, v);
    }
    static void erase(map& m, const std::string& k) { m.erase(k); }
    static const std::string* get(const map& m, const std::string& k)
    {
        auto it = m.find(k);
        return it == m.end() ? nullptr : &it->second;
    }
};

template <>
struct str_ops<std::unordered_map<std::string, std::string, string_hash>>
{
    using map = std::unordered_map<std::string, std::string, string_hash>;

    static void put(map& m, const std::string& k, const std::string& v)
    {
        m.insert_or_assign(k, v);
    }
    static void erase(map& m, const std::string& k) { m.erase(k); }
    static const std::string* get(const map& m, const std::string& k)
    {
        auto it = m.find(k);
        return it == m.end() ? nullptr : &it->second;
    }
};

struct bench_result
{
    std::uint64_t checksum;
    double median_ms;
    double best_ms;
};

template <typename Fn>
bench_result run_timed(int reps, Fn&& fn)
{
    std::vector<double> times;
    std::uint64_t checksum = 0;
    for (int r = 0; r < reps; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        checksum = fn();
        const auto t1 = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(times.begin(), times.end());
    return bench_result{checksum, times[times.size() / 2], times.front()};
}

volatile std::uint64_t g_sink = 0;

void report(std::string_view bench, std::string_view impl, const bench_result& r)
{
    std::printf("%-28s %-18s %10.2f ms  (best %10.2f ms)  checksum %016llx\n",
                std::string(bench).c_str(), std::string(impl).c_str(), r.median_ms,
                r.best_ms, static_cast<unsigned long long>(r.checksum));
    g_sink ^= r.checksum;
}

template <typename Map>
std::uint64_t random_insert_access(std::uint64_t iters, std::uint64_t bound)
{
    using ops = u64_ops<Map>;
    typename ops::map m;
    pcg32 rng;
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iters; ++i) {
        const std::uint64_t k = rng.bounded(bound);
        const std::uint64_t* v = ops::get(m, k);
        if (v == nullptr) {
            ops::put(m, k, 0);
        } else {
            sum += *v;
            ops::put(m, k, *v + 1);
        }
    }
    return sum ^ m.size();
}

template <typename Map>
std::uint64_t random_insert_erase(std::uint64_t iters, std::uint64_t mask)
{
    using ops = u64_ops<Map>;
    typename ops::map m;
    pcg32 rng;
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < iters; ++i) {
        ops::put(m, rng.next64() & mask, i);
        ops::erase(m, rng.next64() & mask);
        sum += m.size();
    }
    return sum;
}

template <typename Map>
std::uint64_t insert_then_erase(std::uint64_t n)
{
    using ops = u64_ops<Map>;
    typename ops::map m;
    pcg32 rng;
    std::uint64_t sum = 0;
    std::vector<std::uint64_t> keys;
    keys.reserve(n);
    for (std::uint64_t i = 0; i < n; ++i) {
        const std::uint64_t k = rng.next64();
        keys.push_back(k);
        ops::put(m, k, i);
    }
    sum += m.size();
    m.clear();
    for (std::uint64_t i = 0; i < n; ++i) {
        const std::uint64_t k = rng.next64();
        keys[i] = k;
        ops::put(m, k, i);
    }
    sum += m.size();
    for (const std::uint64_t k : keys)
        ops::erase(m, k);
    return sum ^ m.size();
}

template <typename Map>
std::uint64_t iterate(std::uint64_t n)
{
    using ops = u64_ops<Map>;
    typename ops::map m;
    pcg32 rng;
    std::uint64_t sum = 0;
    const auto state0 = rng.state;
    for (std::uint64_t i = 0; i < n; ++i) {
        ops::put(m, rng.next64(), i);
        for (const auto& [k, v] : m) {
            (void)k;
            sum += v;
        }
    }
    rng.state = state0;
    for (std::uint64_t i = 0; i < n; ++i) {
        ops::erase(m, rng.next64());
        for (const auto& [k, v] : m) {
            (void)k;
            sum += v;
        }
    }
    return sum ^ m.size();
}

template <typename Map>
std::uint64_t find_bench(std::uint64_t target, std::uint64_t lookups_per_block,
                         double hit_rate)
{
    using ops = u64_ops<Map>;
    typename ops::map m;
    pcg32 ins_rng;
    pcg32 lookup_rng;
    std::uint64_t found = 0;
    std::uint64_t sum = 0;
    while (m.size() < target) {
        std::uint64_t keys[4];
        for (int i = 0; i < 4; ++i) {
            const bool present =
                ins_rng.bounded(100) < static_cast<std::uint64_t>(hit_rate * 100);
            keys[i] = present ? lookup_rng.next64() : ins_rng.next64();
        }
        for (int i = 3; i > 0; --i) {
            const std::uint64_t j =
                ins_rng.bounded(static_cast<std::uint64_t>(i) + 1);
            std::swap(keys[i], keys[j]);
        }
        for (const std::uint64_t k : keys) {
            if (ops::get(m, k) == nullptr)
                ops::put(m, k, k);
        }
        for (std::uint64_t i = 0; i < lookups_per_block; ++i) {
            const std::uint64_t k = lookup_rng.next64();
            const std::uint64_t* v = ops::get(m, k);
            if (v != nullptr) {
                ++found;
                sum += *v;
            }
        }
    }
    return (sum ^ found ^ (m.size() << 32)) + 1;
}

template <typename Map>
std::uint64_t duplicate_bench(Map& m, int copies)
{
    using ops = u64_ops<Map>;
    std::uint64_t sum = 0;
    for (int i = 0; i < copies; ++i) {
        auto copy = ops::duplicate(m);
        sum += copy.size();
    }
    return sum ^ m.size();
}

template <typename Map>
std::uint64_t string_insert_erase(std::uint64_t iters, std::size_t len)
{
    using ops = str_ops<Map>;
    typename ops::map m;
    pcg32 rng;
    std::string str(len, 'x');
    const std::size_t tail = (std::min)(len, static_cast<std::size_t>(4));
    std::uint64_t verifier = 0;
    for (std::uint64_t i = 0; i < iters; ++i) {
        for (std::size_t j = len - tail; j < len; ++j)
            str[j] = static_cast<char>('a' + rng.bounded(26));
        ops::put(m, str, str);
        for (std::size_t j = len - tail; j < len; ++j)
            str[j] = static_cast<char>('a' + rng.bounded(26));
        if (ops::get(m, str) != nullptr) {
            ++verifier;
            ops::erase(m, str);
        }
    }
    return verifier ^ m.size();
}

template <typename Map>
std::uint64_t string_find(std::uint64_t target, std::uint64_t lookups_per_block,
                          double hit_rate, std::size_t len)
{
    using ops = str_ops<Map>;
    typename ops::map m;
    pcg32 ins_rng;
    pcg32 lookup_rng;
    std::string buf(len, 'x');
    const std::size_t tail = (std::min)(len, static_cast<std::size_t>(4));
    const auto fill = [&](pcg32& r) {
        for (std::size_t j = len - tail; j < len; ++j)
            buf[j] = static_cast<char>('a' + r.bounded(26));
    };
    std::uint64_t found = 0;
    std::uint64_t sum = 0;
    while (m.size() < target) {
        for (int i = 0; i < 4; ++i) {
            fill(ins_rng);
            if (ops::get(m, buf) == nullptr)
                ops::put(m, buf, buf);
        }
        for (std::uint64_t i = 0; i < lookups_per_block; ++i) {
            if (lookup_rng.bounded(100) < static_cast<std::uint64_t>(hit_rate * 100))
                fill(ins_rng);
            else
                fill(lookup_rng);
            if (ops::get(m, buf) != nullptr) {
                ++found;
                sum += static_cast<std::uint64_t>(buf.back());
            }
        }
    }
    return (sum ^ found ^ (m.size() << 32)) + 1;
}

} // namespace

int main(int argc, char** argv)
{
    double scale = 1.0;
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0)
            scale = 0.1;
        else
            filter = argv[i];
    }

    const int reps = 3;
    const auto N = [&](std::uint64_t v) {
        return (std::max)(static_cast<std::uint64_t>(1),
                          static_cast<std::uint64_t>(v * scale));
    };
    const auto run = [&](std::string_view name) {
        if (filter == nullptr)
            return true;
        const std::string_view f(filter);
        return name.find(f) != std::string_view::npos || f.find(name) == 0;
    };

    using hamt_u64 = hamt::hamt_map<std::uint64_t, std::uint64_t, int_hash>;
    using std_u64 = std::unordered_map<std::uint64_t, std::uint64_t, int_hash>;
    using hamt_str = hamt::hamt_map<std::string, std::string, string_hash>;
    using std_str = std::unordered_map<std::string, std::string, string_hash>;

    if (run("insert_access")) {
        struct { const char* name; std::uint64_t bound; } variants[] = {
            {"insert_access/5%", 250000},
            {"insert_access/25%", 12500000},
            {"insert_access/50%", 25000000},
        };
        for (const auto& v : variants) {
            if (!run(v.name))
                continue;
            const std::uint64_t iters = N(10000000);
            report(v.name, "hamt_map",
                   run_timed(reps, [&] { return random_insert_access<hamt_u64>(iters, v.bound); }));
            report(v.name, "std::unordered_map",
                   run_timed(reps, [&] { return random_insert_access<std_u64>(iters, v.bound); }));
        }
    }

    if (run("insert_erase")) {
        const std::uint64_t masks[] = {
            0x9000000000000008ull, 0x9020300000001408ull, 0x9006300010010010ull,
            0x9006300110170128ull, 0xD806301010171129ull, 0xD8E6309210971129ull,
        };
        for (std::size_t i = 0; i < std::size(masks); ++i) {
            char label[32];
            std::snprintf(label, sizeof(label), "insert_erase/%zu", i + 1);
            if (!run(label))
                continue;
            const std::uint64_t iters = N(5000000);
            report(label, "hamt_map",
                   run_timed(reps, [&] { return random_insert_erase<hamt_u64>(iters, masks[i]); }));
            report(label, "std::unordered_map",
                   run_timed(reps, [&] { return random_insert_erase<std_u64>(iters, masks[i]); }));
        }
    }

    if (run("insert_then_erase")) {
        const std::uint64_t n = N(1000000);
        report("insert_then_erase", "hamt_map",
               run_timed(reps, [&] { return insert_then_erase<hamt_u64>(n); }));
        report("insert_then_erase", "std::unordered_map",
               run_timed(reps, [&] { return insert_then_erase<std_u64>(n); }));
    }

    if (run("iterate")) {
        const std::uint64_t n = N(5000);
        report("iterate", "hamt_map",
               run_timed(reps, [&] { return iterate<hamt_u64>(n); }));
        report("iterate", "std::unordered_map",
               run_timed(reps, [&] { return iterate<std_u64>(n); }));
    }

    if (run("find")) {
        struct { const char* name; std::uint64_t target; std::uint64_t lookups; } variants[] = {
            {"find/200", 200, 30000},
            {"find/2000", 2000, 6000},
            {"find/500k", 500000, 100},
        };
        const double rates[] = {0.0, 0.25, 0.5, 0.75, 1.0};
        for (const auto& v : variants) {
            for (const double r : rates) {
                char label[48];
                std::snprintf(label, sizeof(label), "%s/%.0f%%", v.name, r * 100);
                if (!run(label))
                    continue;
                report(label, "hamt_map",
                       run_timed(reps, [&] { return find_bench<hamt_u64>(v.target, N(v.lookups), r); }));
                report(label, "std::unordered_map",
                       run_timed(reps, [&] { return find_bench<std_u64>(v.target, N(v.lookups), r); }));
            }
        }
    }

    if (run("duplicate")) {
        const std::uint64_t n = N(1000000);
        hamt_u64 hamt_src;
        std_u64 std_src;
        {
            pcg32 rng;
            for (std::uint64_t i = 0; i < n; ++i) {
                const std::uint64_t k = rng.next64();
                hamt_src.insert(k, i);
                std_src.insert_or_assign(k, i);
            }
        }
        report("fork", "hamt_map",
               run_timed(reps, [&] { return duplicate_bench<hamt_u64>(hamt_src, 200); }));
        report("copy", "std::unordered_map",
               run_timed(reps, [&] { return duplicate_bench<std_u64>(std_src, 20); }));
    }

    if (run("string_insert_erase")) {
        struct { std::size_t len; std::uint64_t iters; } variants[] = {
            {7, 5000000}, {8, 5000000}, {13, 5000000}, {100, 2000000}, {1000, 500000},
        };
        for (const auto& v : variants) {
            char label[48];
            std::snprintf(label, sizeof(label), "string_insert_erase/%zu", v.len);
            if (!run(label))
                continue;
            report(label, "hamt_map",
                   run_timed(reps, [&] { return string_insert_erase<hamt_str>(N(v.iters), v.len); }));
            report(label, "std::unordered_map",
                   run_timed(reps, [&] { return string_insert_erase<std_str>(N(v.iters), v.len); }));
        }
    }

    if (run("string_find")) {
        const double rates[] = {0.0, 0.25, 0.5, 0.75, 1.0};
        struct { const char* name; std::uint64_t target; std::uint64_t lookups; std::size_t len; } variants[] = {
            {"string_find/20k", 20000, 200, 100},
            {"string_find/100k", 100000, 50, 13},
        };
        for (const auto& v : variants) {
            for (const double r : rates) {
                char label[64];
                std::snprintf(label, sizeof(label), "%s/%.0f%%", v.name, r * 100);
                if (!run(label))
                    continue;
                report(label, "hamt_map",
                       run_timed(reps, [&] { return string_find<hamt_str>(v.target, N(v.lookups), r, v.len); }));
                report(label, "std::unordered_map",
                       run_timed(reps, [&] { return string_find<std_str>(v.target, N(v.lookups), r, v.len); }));
            }
        }
    }

    return 0;
}
