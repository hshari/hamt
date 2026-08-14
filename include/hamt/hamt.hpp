#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace hamt {

namespace detail {

constexpr std::uint64_t mix64(std::uint64_t z) noexcept
{
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

inline constexpr std::size_t k_wide_levels = 12;
inline constexpr std::size_t k_levels = k_wide_levels + 1;

constexpr std::uint32_t shift_for(std::size_t depth) noexcept
{
    return depth < k_wide_levels ? static_cast<std::uint32_t>(59 - 5 * depth) : 0u;
}

constexpr std::uint32_t mask_for(std::size_t depth) noexcept
{
    return depth < k_wide_levels ? 0x1Fu : 0x0Fu;
}

constexpr std::uint32_t slot_for(std::uint64_t hash, std::size_t depth) noexcept
{
    return static_cast<std::uint32_t>((hash >> shift_for(depth)) & mask_for(depth));
}

constexpr std::uint32_t bit_for(std::uint64_t hash, std::size_t depth) noexcept
{
    return std::uint32_t{1} << slot_for(hash, depth);
}

template <typename Key, typename Value>
struct map_traits
{
    using entry = std::pair<Key, Value>;
    using exposed = std::pair<const Key, Value>;
    using mapped_type = Value;

    static constexpr bool updates_value = true;

    static constexpr bool value_comparable = requires(const Value& a, const Value& b) {
        { a == b } -> std::convertible_to<bool>;
    };

    static const Key& key_of(const entry& e) noexcept
    {
        return e.first;
    }

    static const Key& key_of(const exposed& e) noexcept
    {
        return e.first;
    }

    static const exposed& to_exposed(const entry& e) noexcept
    {
        return *std::launder(reinterpret_cast<const exposed*>(std::addressof(e)));
    }

    static bool same_value(const Value& a, const Value& b)
    {
        return a == b;
    }

    template <typename V>
    static entry replace_value(const entry& existing, V&& v)
    {
        return entry(existing.first, std::forward<V>(v));
    }

    template <typename V>
    static void assign_value(entry& e, V&& v)
    {
        e.second = std::forward<V>(v);
    }
};

template <typename Key>
struct set_traits
{
    using entry = Key;
    using exposed = Key;
    using mapped_type = void;

    static constexpr bool updates_value = false;

    static constexpr bool value_comparable = false;

    static const Key& key_of(const entry& e) noexcept
    {
        return e;
    }

    static const exposed& to_exposed(const entry& e) noexcept
    {
        return e;
    }
};

template <typename Key, typename Hash, typename Equal, typename Traits, typename Derived>
class hamt_common
{
public:
    using key_type = Key;
    using value_type = typename Traits::exposed;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using hash_type = Hash;
    using equal_type = Equal;

protected:
    using entry_type = typename Traits::entry;
    using exposed_type = typename Traits::exposed;

    class node_ptr;

    struct node
    {
        enum class kind_t : std::uint8_t { leaf, collision, bitmap };
        const kind_t kind;
        const std::uint64_t gen;
        std::uint32_t refs;
        node(kind_t k, std::uint64_t g) noexcept : kind(k), gen(g), refs(1) {}
        node(const node&) = delete;
        node& operator=(const node&) = delete;
    };

    struct leaf_node final : node
    {
        const std::uint64_t hash;
        entry_type entry;
        leaf_node(std::uint64_t g, std::uint64_t h, entry_type e)
            : node(node::kind_t::leaf, g), hash(h), entry(std::move(e)) {}
    };

    struct collision_node final : node
    {
        const std::uint64_t hash;
        std::vector<entry_type> entries;
        collision_node(std::uint64_t g, std::uint64_t h, std::vector<entry_type> es)
            : node(node::kind_t::collision, g), hash(h), entries(std::move(es)) {}
    };

    struct bitmap_node final : node
    {
        std::uint32_t bitmap;
        bitmap_node(std::uint64_t g, std::uint32_t b)
            : node(node::kind_t::bitmap, g), bitmap(b) {}

        node_ptr* children() noexcept
        {
            return reinterpret_cast<node_ptr*>(this + 1);
        }

        const node_ptr* children() const noexcept
        {
            return reinterpret_cast<const node_ptr*>(this + 1);
        }

        std::size_t count() const noexcept
        {
            return static_cast<std::size_t>(std::popcount(bitmap));
        }
    };

    class node_ptr
    {
    public:
        node_ptr() noexcept = default;
        node_ptr(std::nullptr_t) noexcept {}
        explicit node_ptr(node* p) noexcept : p_(p) {}

        node_ptr(const node_ptr& other) noexcept : p_(other.p_)
        {
            if (p_ != nullptr)
                ++p_->refs;
        }

        node_ptr(node_ptr&& other) noexcept : p_(other.p_)
        {
            other.p_ = nullptr;
        }

        node_ptr& operator=(const node_ptr& other) noexcept
        {
            if (this != &other) {
                release();
                p_ = other.p_;
                if (p_ != nullptr)
                    ++p_->refs;
            }
            return *this;
        }

        node_ptr& operator=(node_ptr&& other) noexcept
        {
            if (this != &other) {
                release();
                p_ = other.p_;
                other.p_ = nullptr;
            }
            return *this;
        }

        ~node_ptr()
        {
            release();
        }

        node* get() const noexcept { return p_; }
        node& operator*() const noexcept { return *p_; }
        node* operator->() const noexcept { return p_; }
        explicit operator bool() const noexcept { return p_ != nullptr; }

        void reset(node* p = nullptr) noexcept
        {
            if (p_ != p) {
                release();
                p_ = p;
            }
        }

        friend bool operator==(const node_ptr& a, const node_ptr& b) noexcept
        {
            return a.p_ == b.p_;
        }

        friend bool operator!=(const node_ptr& a, const node_ptr& b) noexcept
        {
            return a.p_ != b.p_;
        }

    private:
        static void destroy(node* n) noexcept
        {
            switch (n->kind) {
            case node::kind_t::leaf: {
                leaf_node* l = static_cast<leaf_node*>(n);
                l->~leaf_node();
                ::operator delete(l);
                break;
            }
            case node::kind_t::collision: {
                collision_node* c = static_cast<collision_node*>(n);
                c->~collision_node();
                ::operator delete(c);
                break;
            }
            case node::kind_t::bitmap: {
                bitmap_node* b = static_cast<bitmap_node*>(n);
                node_ptr* ch = b->children();
                const std::size_t count = b->count();
                for (std::size_t i = 0; i < count; ++i)
                    ch[i].~node_ptr();
                b->~bitmap_node();
                ::operator delete(b);
                break;
            }
            }
        }

        void release() noexcept
        {
            if (p_ != nullptr && --p_->refs == 0)
                destroy(p_);
        }

        node* p_ = nullptr;
    };

    static leaf_node* as_leaf(const node_ptr& n) noexcept
    {
        return static_cast<leaf_node*>(n.get());
    }

    static collision_node* as_collision(const node_ptr& n) noexcept
    {
        return static_cast<collision_node*>(n.get());
    }

    static bitmap_node* as_bitmap(const node_ptr& n) noexcept
    {
        return static_cast<bitmap_node*>(n.get());
    }

    static const leaf_node* as_leaf(const node* n) noexcept
    {
        return static_cast<const leaf_node*>(n);
    }

    static const collision_node* as_collision(const node* n) noexcept
    {
        return static_cast<const collision_node*>(n);
    }

    static const bitmap_node* as_bitmap(const node* n) noexcept
    {
        return static_cast<const bitmap_node*>(n);
    }

    static std::uint64_t hash_of(const Key& k)
    {
        return detail::mix64(static_cast<std::uint64_t>(Hash{}(k)));
    }

    static node_ptr make_leaf(std::uint64_t gen, std::uint64_t h, entry_type e)
    {
        return node_ptr(new leaf_node(gen, h, std::move(e)));
    }

    static node_ptr make_collision(std::uint64_t gen, std::uint64_t h, std::vector<entry_type> es)
    {
        return node_ptr(new collision_node(gen, h, std::move(es)));
    }

    static node_ptr make_bitmap_single(std::uint64_t gen, std::uint32_t bitmap, node_ptr child)
    {
        void* mem = ::operator new(sizeof(bitmap_node) + sizeof(node_ptr));
        bitmap_node* bm = ::new (mem) bitmap_node(gen, bitmap);
        ::new (bm->children()) node_ptr(std::move(child));
        return node_ptr(bm);
    }

    static node_ptr make_bitmap_insert(std::uint64_t gen, const bitmap_node* bm,
                                       std::size_t pos, std::uint32_t bit, node_ptr new_child)
    {
        const std::size_t count = bm->count();
        void* mem = ::operator new(sizeof(bitmap_node) + (count + 1) * sizeof(node_ptr));
        bitmap_node* out = ::new (mem) bitmap_node(gen, bm->bitmap | bit);
        node_ptr* dst = out->children();
        const node_ptr* src = bm->children();
        for (std::size_t i = 0; i < pos; ++i)
            ::new (dst + i) node_ptr(src[i]);
        ::new (dst + pos) node_ptr(std::move(new_child));
        for (std::size_t i = pos; i < count; ++i)
            ::new (dst + i + 1) node_ptr(src[i]);
        return node_ptr(out);
    }

    static node_ptr make_bitmap_replace(std::uint64_t gen, const bitmap_node* bm,
                                        std::size_t pos, node_ptr new_child)
    {
        const std::size_t count = bm->count();
        void* mem = ::operator new(sizeof(bitmap_node) + count * sizeof(node_ptr));
        bitmap_node* out = ::new (mem) bitmap_node(gen, bm->bitmap);
        node_ptr* dst = out->children();
        const node_ptr* src = bm->children();
        for (std::size_t i = 0; i < pos; ++i)
            ::new (dst + i) node_ptr(src[i]);
        ::new (dst + pos) node_ptr(std::move(new_child));
        for (std::size_t i = pos + 1; i < count; ++i)
            ::new (dst + i) node_ptr(src[i]);
        return node_ptr(out);
    }

    static node_ptr make_bitmap_erase(std::uint64_t gen, const bitmap_node* bm,
                                      std::size_t pos, std::uint32_t bit)
    {
        const std::size_t count = bm->count();
        void* mem = ::operator new(sizeof(bitmap_node) + (count - 1) * sizeof(node_ptr));
        bitmap_node* out = ::new (mem) bitmap_node(gen, bm->bitmap & ~bit);
        node_ptr* dst = out->children();
        const node_ptr* src = bm->children();
        for (std::size_t i = 0; i < pos; ++i)
            ::new (dst + i) node_ptr(src[i]);
        for (std::size_t i = pos + 1; i < count; ++i)
            ::new (dst + i - 1) node_ptr(src[i]);
        return node_ptr(out);
    }

    static void bitmap_erase_in_place(bitmap_node* bm, std::size_t pos) noexcept
    {
        const std::size_t count = bm->count();
        node_ptr* ch = bm->children();
        for (std::size_t i = pos; i + 1 < count; ++i)
            ch[i] = std::move(ch[i + 1]);
        ch[count - 1].~node_ptr();
    }

    static node_ptr ins(node_ptr n, std::uint64_t h, std::size_t depth,
                        entry_type e, std::uint64_t gen, bool& added, bool& changed)
    {
        const Key& k = Traits::key_of(e);
        switch (n->kind) {
        case node::kind_t::leaf: {
            leaf_node* leaf = as_leaf(n);
            if (Equal{}(k, Traits::key_of(leaf->entry))) {
                if constexpr (Traits::updates_value) {
                    if constexpr (Traits::value_comparable) {
                        if (Traits::same_value(leaf->entry.second, e.second))
                            return n;
                    }
                    changed = true;
                    if (leaf->gen == gen) {
                        Traits::assign_value(leaf->entry, std::move(e).second);
                        return n;
                    }
                    return make_leaf(gen, leaf->hash,
                                     Traits::replace_value(leaf->entry, std::move(e).second));
                } else {
                    return n;
                }
            }
            if (h == leaf->hash) {
                added = true;
                changed = true;
                std::vector<entry_type> es;
                es.reserve(2);
                es.push_back(std::move(leaf->entry));
                es.push_back(std::move(e));
                return make_collision(gen, h, std::move(es));
            }
            assert(depth < detail::k_levels);
            std::uint32_t bit = detail::bit_for(leaf->hash, depth);
            node_ptr b = make_bitmap_single(gen, bit, std::move(n));
            return ins(std::move(b), h, depth, std::move(e), gen, added, changed);
        }
        case node::kind_t::collision: {
            collision_node* col = as_collision(n);
            const auto it = std::find_if(col->entries.begin(), col->entries.end(),
                                         [&](const entry_type& x) { return Equal{}(k, Traits::key_of(x)); });
            if (it != col->entries.end()) {
                if constexpr (Traits::updates_value) {
                    if constexpr (Traits::value_comparable) {
                        if (Traits::same_value(it->second, e.second))
                            return n;
                    }
                    changed = true;
                    if (col->gen == gen) {
                        Traits::assign_value(*it, std::move(e).second);
                        return n;
                    }
                    const std::size_t idx =
                        static_cast<std::size_t>(std::distance(col->entries.begin(), it));
                    auto clone = make_collision(gen, col->hash, col->entries);
                    Traits::assign_value(as_collision(clone)->entries[idx], std::move(e).second);
                    return clone;
                } else {
                    return n;
                }
            }
            if (h == col->hash) {
                added = true;
                changed = true;
                if (col->gen == gen) {
                    col->entries.push_back(std::move(e));
                    return n;
                }
                auto clone = make_collision(gen, col->hash, col->entries);
                as_collision(clone)->entries.push_back(std::move(e));
                return clone;
            }
            assert(depth < detail::k_levels);
            std::uint32_t bit = detail::bit_for(col->hash, depth);
            node_ptr b = make_bitmap_single(gen, bit, std::move(n));
            return ins(std::move(b), h, depth, std::move(e), gen, added, changed);
        }
        case node::kind_t::bitmap: {
            bitmap_node* bm = as_bitmap(n);
            const std::uint32_t bit = detail::bit_for(h, depth);
            const std::size_t pos =
                static_cast<std::size_t>(std::popcount(bm->bitmap & (bit - 1)));
            if ((bm->bitmap & bit) == 0) {
                added = true;
                changed = true;
                node_ptr new_leaf = make_leaf(gen, h, std::move(e));
                return make_bitmap_insert(gen, bm, pos, bit, std::move(new_leaf));
            }
            node_ptr new_child = ins(bm->children()[pos], h, depth + 1,
                                     std::move(e), gen, added, changed);
            if (new_child == bm->children()[pos])
                return n;
            if (bm->gen == gen) {
                bm->children()[pos] = std::move(new_child);
                return n;
            }
            return make_bitmap_replace(gen, bm, pos, std::move(new_child));
        }
        }
        return n;
    }

    static node_ptr del(node_ptr n, std::uint64_t h, std::size_t depth,
                        const Key& k, std::uint64_t gen, bool& erased)
    {
        switch (n->kind) {
        case node::kind_t::leaf: {
            const leaf_node* leaf = as_leaf(n);
            if (!Equal{}(k, Traits::key_of(leaf->entry)))
                return n;
            erased = true;
            return nullptr;
        }
        case node::kind_t::collision: {
            collision_node* col = as_collision(n);
            const auto it = std::find_if(col->entries.begin(), col->entries.end(),
                                         [&](const entry_type& x) { return Equal{}(k, Traits::key_of(x)); });
            if (it == col->entries.end())
                return n;
            erased = true;
            if (col->entries.size() == 1)
                return nullptr;
            if (col->entries.size() == 2) {
                const std::size_t keep = it == col->entries.begin() ? 1 : 0;
                return make_leaf(gen, col->hash, col->entries[keep]);
            }
            if (col->gen == gen) {
                col->entries.erase(it);
                return n;
            }
            const std::size_t idx =
                static_cast<std::size_t>(std::distance(col->entries.begin(), it));
            auto clone = make_collision(gen, col->hash, col->entries);
            as_collision(clone)->entries.erase(
                as_collision(clone)->entries.begin() + static_cast<std::ptrdiff_t>(idx));
            return clone;
        }
        case node::kind_t::bitmap: {
            bitmap_node* bm = as_bitmap(n);
            const std::uint32_t bit = detail::bit_for(h, depth);
            if ((bm->bitmap & bit) == 0)
                return n;
            const std::size_t pos =
                static_cast<std::size_t>(std::popcount(bm->bitmap & (bit - 1)));
            node_ptr new_child = del(bm->children()[pos], h, depth + 1, k, gen, erased);
            if (!erased)
                return n;
            if (new_child == bm->children()[pos])
                return n;
            if (new_child) {
                if (bm->gen == gen) {
                    bm->children()[pos] = std::move(new_child);
                    return n;
                }
                return make_bitmap_replace(gen, bm, pos, std::move(new_child));
            }
            const std::size_t count = bm->count();
            if (count == 1)
                return nullptr;
            if (count == 2) {
                const std::size_t keep = pos == 0 ? 1 : 0;
                if (bm->children()[keep]->kind != node::kind_t::bitmap)
                    return bm->children()[keep];
            }
            if (bm->gen == gen) {
                bitmap_erase_in_place(bm, pos);
                bm->bitmap &= ~bit;
                return n;
            }
            return make_bitmap_erase(gen, bm, pos, bit);
        }
        }
        return n;
    }

    static bool find_in(const node* cur, std::uint64_t h, const Key& k)
    {
        std::size_t depth = 0;
        for (;;) {
            switch (cur->kind) {
            case node::kind_t::bitmap: {
                const bitmap_node* bm = as_bitmap(cur);
                const std::uint32_t bit = detail::bit_for(h, depth);
                if ((bm->bitmap & bit) == 0)
                    return false;
                const std::size_t pos =
                    static_cast<std::size_t>(std::popcount(bm->bitmap & (bit - 1)));
                cur = bm->children()[pos].get();
                ++depth;
                break;
            }
            case node::kind_t::leaf: {
                const leaf_node* leaf = as_leaf(cur);
                return Equal{}(k, Traits::key_of(leaf->entry));
            }
            case node::kind_t::collision: {
                const collision_node* col = as_collision(cur);
                for (const auto& x : col->entries) {
                    if (Equal{}(k, Traits::key_of(x)))
                        return true;
                }
                return false;
            }
            }
        }
    }

    struct frame
    {
        const bitmap_node* parent;
        std::size_t index;
        bool operator==(const frame& other) const noexcept
        {
            return parent == other.parent && index == other.index;
        }
    };

    static std::pair<std::vector<frame>, const node*> descend(const node* n)
    {
        std::vector<frame> path;
        while (n->kind == node::kind_t::bitmap) {
            const bitmap_node* bm = as_bitmap(n);
            path.push_back(frame{bm, 0});
            n = bm->children()[0].get();
        }
        return {std::move(path), n};
    }

public:
    class const_iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using iterator_concept = std::forward_iterator_tag;
        using value_type = exposed_type;
        using difference_type = std::ptrdiff_t;
        using pointer = const exposed_type*;
        using reference = const exposed_type&;

        const_iterator() noexcept = default;

        const_iterator(const const_iterator&) = default;

        const_iterator& operator=(const const_iterator&) = default;

        reference operator*() const
        {
            if (node_->kind == node::kind_t::leaf)
                return Traits::to_exposed(as_leaf(node_)->entry);
            assert(node_->kind == node::kind_t::collision);
            return Traits::to_exposed(as_collision(node_)->entries[entry_]);
        }

        pointer operator->() const
        {
            return std::addressof(**this);
        }

        const_iterator& operator++()
        {
            if (node_ != nullptr && node_->kind == node::kind_t::collision) {
                const collision_node* col = as_collision(node_);
                if (entry_ + 1 < col->entries.size()) {
                    ++entry_;
                    return *this;
                }
            }
            while (!path_.empty()) {
                frame& f = path_.back();
                const bitmap_node* bm = f.parent;
                if (f.index + 1 < bm->count()) {
                    ++f.index;
                    const node* n = bm->children()[f.index].get();
                    while (n->kind == node::kind_t::bitmap) {
                        const bitmap_node* b = as_bitmap(n);
                        path_.push_back(frame{b, 0});
                        n = b->children()[0].get();
                    }
                    node_ = n;
                    entry_ = 0;
                    return *this;
                }
                path_.pop_back();
            }
            node_ = nullptr;
            path_.clear();
            entry_ = 0;
            return *this;
        }

        const_iterator operator++(int)
        {
            const_iterator tmp = *this;
            ++*this;
            return tmp;
        }

        friend bool operator==(const const_iterator& a, const const_iterator& b) noexcept
        {
            if (a.node_ == nullptr || b.node_ == nullptr)
                return a.node_ == b.node_;
            return a.node_ == b.node_ && a.entry_ == b.entry_ && a.path_ == b.path_;
        }

        friend bool operator!=(const const_iterator& a, const const_iterator& b) noexcept
        {
            return !(a == b);
        }

    private:
        friend hamt_common;

        node_ptr root_;
        std::vector<frame> path_;
        const node* node_ = nullptr;
        std::size_t entry_ = 0;

        const_iterator(node_ptr root, std::vector<frame> path, const node* n, std::size_t entry)
            : root_(std::move(root)), path_(std::move(path)), node_(n), entry_(entry) {}

        [[nodiscard]] std::uint64_t hash() const noexcept
        {
            return node_->kind == node::kind_t::leaf
                       ? as_leaf(node_)->hash
                       : as_collision(node_)->hash;
        }
    };

    using iterator = const_iterator;

    [[nodiscard]] size_type size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return size_ == 0;
    }

    [[nodiscard]] Derived fork() noexcept
    {
        assert(gen_ != (std::numeric_limits<std::uint64_t>::max)());
        ++gen_;
        return Derived(root_, size_, gen_);
    }

    [[nodiscard]] const_iterator begin() const
    {
        if (root_ == nullptr)
            return const_iterator();
        auto [path, n] = descend(root_.get());
        return const_iterator(root_, std::move(path), n, 0);
    }

    [[nodiscard]] const_iterator end() const noexcept
    {
        return const_iterator();
    }

    [[nodiscard]] const_iterator find(const Key& k) const
    {
        if (root_ == nullptr)
            return const_iterator();
        const std::uint64_t h = hash_of(k);
        const node* cur = root_.get();
        std::size_t depth = 0;
        std::vector<frame> path;
        for (;;) {
            switch (cur->kind) {
            case node::kind_t::bitmap: {
                const bitmap_node* bm = as_bitmap(cur);
                const std::uint32_t bit = detail::bit_for(h, depth);
                if ((bm->bitmap & bit) == 0)
                    return const_iterator();
                const std::size_t pos =
                    static_cast<std::size_t>(std::popcount(bm->bitmap & (bit - 1)));
                path.push_back(frame{bm, pos});
                cur = bm->children()[pos].get();
                ++depth;
                break;
            }
            case node::kind_t::leaf: {
                const leaf_node* leaf = as_leaf(cur);
                if (Equal{}(k, Traits::key_of(leaf->entry)))
                    return const_iterator(root_, std::move(path), leaf, 0);
                return const_iterator();
            }
            case node::kind_t::collision: {
                const collision_node* col = as_collision(cur);
                std::size_t i = 0;
                for (const auto& x : col->entries) {
                    if (Equal{}(k, Traits::key_of(x)))
                        return const_iterator(root_, std::move(path), col, i);
                    ++i;
                }
                return const_iterator();
            }
            }
        }
    }

    [[nodiscard]] bool contains(const Key& k) const
    {
        if (root_ == nullptr)
            return false;
        return find_in(root_.get(), hash_of(k), k);
    }

    [[nodiscard]] size_type count(const Key& k) const
    {
        return contains(k) ? 1 : 0;
    }

    void clear() noexcept
    {
        root_.reset();
        size_ = 0;
    }

    void swap(Derived& other) noexcept
    {
        std::swap(root_, other.root_);
        std::swap(size_, other.size_);
        std::swap(gen_, other.gen_);
    }

protected:
    hamt_common() = default;

    hamt_common(node_ptr root, size_type size, std::uint64_t gen)
        : root_(std::move(root)), size_(size), gen_(gen) {}

    ~hamt_common() = default;

    hamt_common(const hamt_common&) = delete;
    hamt_common& operator=(const hamt_common&) = delete;

    hamt_common(hamt_common&& other) noexcept
        : root_(std::move(other.root_)), size_(other.size_), gen_(other.gen_)
    {
        other.reset_tracking();
    }

    hamt_common& operator=(hamt_common&& other) noexcept
    {
        assign_from(other);
        return *this;
    }

    void assign_from(hamt_common& other) noexcept
    {
        if (this != &other) {
            root_ = std::move(other.root_);
            size_ = other.size_;
            gen_ = other.gen_;
            other.reset_tracking();
        }
    }

    Derived& insert_entry(entry_type e)
    {
        const std::uint64_t h = hash_of(Traits::key_of(e));
        if (root_ == nullptr) {
            root_ = make_leaf(gen_, h, std::move(e));
            ++size_;
            return self();
        }
        bool added = false;
        bool changed = false;
        node_ptr nr = ins(root_, h, 0, std::move(e), gen_, added, changed);
        if (nr != root_)
            root_ = std::move(nr);
        if (changed)
            size_ += (added ? 1 : 0);
        return self();
    }

    Derived& erase_entry(const Key& k)
    {
        if (root_ == nullptr)
            return self();
        bool erased = false;
        node_ptr nr = del(root_, hash_of(k), 0, k, gen_, erased);
        if (nr != root_)
            root_ = std::move(nr);
        if (erased)
            --size_;
        return self();
    }

    void reset_tracking() noexcept
    {
        size_ = 0;
        gen_ = 0;
    }

    static bool containers_equal(const Derived& a, const Derived& b)
    {
        if (a.size_ != b.size_)
            return false;
        auto ia = a.begin();
        auto ib = b.begin();
        const auto ea = a.end();
        const auto eb = b.end();
        while (ia != ea && ib != eb) {
            const std::uint64_t h = ia.hash();
            if (ib.hash() != h)
                return false;
            std::vector<const Key*> bkeys;
            std::vector<const typename Traits::mapped_type*> bvals;
            while (ib != eb && ib.hash() == h) {
                bkeys.push_back(std::addressof(Traits::key_of(*ib)));
                if constexpr (Traits::value_comparable)
                    bvals.push_back(std::addressof((*ib).second));
                ++ib;
            }
            while (ia != ea && ia.hash() == h) {
                const Key& ak = Traits::key_of(*ia);
                bool matched = false;
                for (std::size_t j = 0; j < bkeys.size(); ++j) {
                    if (Equal{}(ak, *bkeys[j])) {
                        if constexpr (Traits::value_comparable) {
                            if (!Traits::same_value((*ia).second, *bvals[j]))
                                return false;
                        }
                        matched = true;
                        break;
                    }
                }
                if (!matched)
                    return false;
                ++ia;
            }
        }
        return ia == ea && ib == eb;
    }

private:
    Derived& self() noexcept
    {
        return static_cast<Derived&>(*this);
    }

    node_ptr root_;
    size_type size_ = 0;
    std::uint64_t gen_ = 0;
};

} // namespace detail

template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>>
requires std::copy_constructible<Key> && std::copy_constructible<Value>
    && std::predicate<Equal, const Key&, const Key&>
    && std::invocable<Hash, const Key&>
    && std::convertible_to<std::invoke_result_t<Hash, const Key&>, std::uint64_t>
    && std::is_empty_v<Hash> && std::is_empty_v<Equal>
    && std::default_initializable<Hash> && std::default_initializable<Equal>
class hamt_map
    : public detail::hamt_common<Key, Hash, Equal, detail::map_traits<Key, Value>,
                                 hamt_map<Key, Value, Hash, Equal>>
{
    using base = detail::hamt_common<Key, Hash, Equal, detail::map_traits<Key, Value>,
                                     hamt_map<Key, Value, Hash, Equal>>;
    friend base;

public:
    using key_type = Key;
    using mapped_type = Value;
    using value_type = std::pair<const Key, Value>;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using hash_type = Hash;
    using equal_type = Equal;
    using const_iterator = typename base::const_iterator;
    using iterator = const_iterator;

    hamt_map() = default;

    hamt_map(const hamt_map&) = delete;
    hamt_map& operator=(const hamt_map&) = delete;

    hamt_map(hamt_map&& other) noexcept
        : base(static_cast<base&&>(other))
    {
    }

    hamt_map& operator=(hamt_map&& other) noexcept
    {
        base::assign_from(other);
        return *this;
    }

    hamt_map(std::initializer_list<value_type> init)
    {
        for (const value_type& kv : init)
            insert(kv.first, kv.second);
    }

    template <std::input_iterator It>
    hamt_map(It first, It last)
    {
        for (; first != last; ++first)
            insert(first->first, first->second);
    }

    [[nodiscard]] hamt_map fork() noexcept
    {
        return base::fork();
    }

    hamt_map& insert(Key k, Value v)
    {
        return base::insert_entry(std::make_pair(std::move(k), std::move(v)));
    }

    hamt_map& insert(const value_type& kv)
    {
        return insert(kv.first, kv.second);
    }

    hamt_map& erase(const Key& k)
    {
        return base::erase_entry(k);
    }

    void swap(hamt_map& other) noexcept
    {
        base::swap(other);
    }

    friend void swap(hamt_map& a, hamt_map& b) noexcept
    {
        a.swap(b);
    }

    friend bool operator==(const hamt_map& a, const hamt_map& b)
        requires std::equality_comparable<Value>
    {
        return base::containers_equal(a, b);
    }

    friend bool operator!=(const hamt_map& a, const hamt_map& b)
        requires std::equality_comparable<Value>
    {
        return !(a == b);
    }

private:
    using base::base;
};

template <typename Key,
          typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>>
requires std::copy_constructible<Key>
    && std::predicate<Equal, const Key&, const Key&>
    && std::invocable<Hash, const Key&>
    && std::convertible_to<std::invoke_result_t<Hash, const Key&>, std::uint64_t>
    && std::is_empty_v<Hash> && std::is_empty_v<Equal>
    && std::default_initializable<Hash> && std::default_initializable<Equal>
class hamt_set
    : public detail::hamt_common<Key, Hash, Equal, detail::set_traits<Key>,
                                 hamt_set<Key, Hash, Equal>>
{
    using base = detail::hamt_common<Key, Hash, Equal, detail::set_traits<Key>,
                                     hamt_set<Key, Hash, Equal>>;
    friend base;

public:
    using key_type = Key;
    using value_type = Key;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using hash_type = Hash;
    using equal_type = Equal;
    using const_iterator = typename base::const_iterator;
    using iterator = const_iterator;

    hamt_set() = default;

    hamt_set(const hamt_set&) = delete;
    hamt_set& operator=(const hamt_set&) = delete;

    hamt_set(hamt_set&& other) noexcept
        : base(static_cast<base&&>(other))
    {
    }

    hamt_set& operator=(hamt_set&& other) noexcept
    {
        base::assign_from(other);
        return *this;
    }

    hamt_set(std::initializer_list<value_type> init)
    {
        for (const value_type& k : init)
            insert(k);
    }

    template <std::input_iterator It>
    hamt_set(It first, It last)
    {
        for (; first != last; ++first)
            insert(*first);
    }

    [[nodiscard]] hamt_set fork() noexcept
    {
        return base::fork();
    }

    hamt_set& insert(Key k)
    {
        return base::insert_entry(std::move(k));
    }

    hamt_set& erase(const Key& k)
    {
        return base::erase_entry(k);
    }

    void swap(hamt_set& other) noexcept
    {
        base::swap(other);
    }

    friend void swap(hamt_set& a, hamt_set& b) noexcept
    {
        a.swap(b);
    }

    friend bool operator==(const hamt_set& a, const hamt_set& b)
    {
        return base::containers_equal(a, b);
    }

    friend bool operator!=(const hamt_set& a, const hamt_set& b)
    {
        return !(a == b);
    }

private:
    using base::base;
};

} // namespace hamt
