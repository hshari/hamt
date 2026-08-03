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
#include <memory>
#include <optional>
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

inline constexpr std::size_t k_levels = 12;

constexpr std::uint32_t shift_for(std::size_t depth) noexcept
{
    return depth < k_levels ? static_cast<std::uint32_t>(59 - 5 * depth) : 0u;
}

constexpr std::uint32_t mask_for(std::size_t depth) noexcept
{
    return depth < k_levels ? 0x1Fu : 0x0Fu;
}

constexpr std::uint32_t slot_for(std::uint64_t hash, std::size_t depth) noexcept
{
    return static_cast<std::uint32_t>((hash >> shift_for(depth)) & mask_for(depth));
}

constexpr std::uint32_t bit_for(std::uint64_t hash, std::size_t depth) noexcept
{
    return 1u << slot_for(hash, depth);
}

} // namespace detail

template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>>
requires std::copy_constructible<Key> && std::copy_constructible<Value>
    && std::predicate<Equal, const Key&, const Key&>
    && std::invocable<Hash, const Key&>
    && std::convertible_to<std::invoke_result_t<Hash, const Key&>, std::uint64_t>
class hamt_map
{
public:
    using key_type = Key;
    using mapped_type = Value;
    using value_type = std::pair<const Key, Value>;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using hash_type = Hash;
    using equal_type = Equal;

private:
    struct node;
    using node_ptr = std::shared_ptr<node>;
    using entry_pair = std::pair<Key, Value>;

    struct node
    {
        enum class kind_t : std::uint8_t { leaf, collision, bitmap };
        const kind_t kind;
        const std::uint64_t gen;
        node(kind_t k, std::uint64_t g) noexcept : kind(k), gen(g) {}
        node(const node&) = delete;
        node& operator=(const node&) = delete;
        virtual ~node() = default;
    };

    struct leaf_node final : node
    {
        const std::uint64_t hash;
        const Key key;
        Value value;
        leaf_node(std::uint64_t g, std::uint64_t h, Key k, Value v)
            : node(node::kind_t::leaf, g), hash(h), key(std::move(k)), value(std::move(v)) {}
    };

    struct collision_node final : node
    {
        const std::uint64_t hash;
        std::vector<entry_pair> entries;
        collision_node(std::uint64_t g, std::uint64_t h, std::vector<entry_pair> es)
            : node(node::kind_t::collision, g), hash(h), entries(std::move(es)) {}
    };

    struct bitmap_node final : node
    {
        const std::size_t depth;
        std::uint32_t bitmap;
        std::vector<node_ptr> children;
        bitmap_node(std::uint64_t g, std::size_t d, std::uint32_t b, std::vector<node_ptr> ch)
            : node(node::kind_t::bitmap, g), depth(d), bitmap(b), children(std::move(ch)) {}
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

    static std::uint64_t hash_of(const Key& k)
    {
        return detail::mix64(static_cast<std::uint64_t>(Hash{}(k)));
    }

    static constexpr bool values_comparable_v = requires(const Value& a, const Value& b) {
        { a == b } -> std::convertible_to<bool>;
    };

    struct frame
    {
        node_ptr parent;
        std::size_t index;
        bool operator==(const frame& other) const noexcept
        {
            return parent == other.parent && index == other.index;
        }
    };

    static node_ptr ins(node_ptr n, std::uint64_t h, std::size_t depth,
                        Key k, Value v, std::uint64_t gen, bool& added, bool& changed)
    {
        switch (n->kind) {
        case node::kind_t::leaf: {
            leaf_node* leaf = as_leaf(n);
            if (Equal{}(k, leaf->key)) {
                if constexpr (values_comparable_v) {
                    if (v == leaf->value)
                        return n;
                }
                changed = true;
                if (leaf->gen == gen) {
                    leaf->value = std::move(v);
                    return n;
                }
                return std::make_shared<leaf_node>(gen, leaf->hash, leaf->key, std::move(v));
            }
            if (h == leaf->hash) {
                added = true;
                changed = true;
                std::vector<entry_pair> es;
                es.reserve(2);
                es.emplace_back(leaf->key, leaf->value);
                es.emplace_back(std::move(k), std::move(v));
                return std::make_shared<collision_node>(gen, h, std::move(es));
            }
            assert(depth < detail::k_levels);
            std::uint32_t bit = detail::bit_for(leaf->hash, depth);
            auto b = std::make_shared<bitmap_node>(gen, depth, bit, std::vector<node_ptr>{std::move(n)});
            return ins(std::move(b), h, depth, std::move(k), std::move(v), gen, added, changed);
        }
        case node::kind_t::collision: {
            collision_node* col = as_collision(n);
            const auto it = std::find_if(col->entries.begin(), col->entries.end(),
                                         [&](const entry_pair& e) { return Equal{}(k, e.first); });
            if (it != col->entries.end()) {
                if constexpr (values_comparable_v) {
                    if (it->second == v)
                        return n;
                }
                changed = true;
                if (col->gen == gen) {
                    it->second = std::move(v);
                    return n;
                }
                auto clone = std::make_shared<collision_node>(gen, col->hash, col->entries);
                clone->entries[static_cast<std::size_t>(it - col->entries.begin())].second = std::move(v);
                return clone;
            }
            if (h == col->hash) {
                added = true;
                changed = true;
                if (col->gen == gen) {
                    col->entries.emplace_back(std::move(k), std::move(v));
                    return n;
                }
                auto clone = std::make_shared<collision_node>(gen, col->hash, col->entries);
                clone->entries.emplace_back(std::move(k), std::move(v));
                return clone;
            }
            assert(depth < detail::k_levels);
            std::uint32_t bit = detail::bit_for(col->hash, depth);
            auto b = std::make_shared<bitmap_node>(gen, depth, bit, std::vector<node_ptr>{std::move(n)});
            return ins(std::move(b), h, depth, std::move(k), std::move(v), gen, added, changed);
        }
        case node::kind_t::bitmap: {
            bitmap_node* bm = as_bitmap(n);
            assert(bm->depth == depth);
            const std::uint32_t bit = detail::bit_for(h, depth);
            const std::size_t pos =
                static_cast<std::size_t>(std::popcount(bm->bitmap & (bit - 1)));
            if ((bm->bitmap & bit) == 0) {
                added = true;
                changed = true;
                node_ptr new_leaf = std::make_shared<leaf_node>(gen, h, std::move(k), std::move(v));
                if (bm->gen == gen) {
                    bm->children.insert(bm->children.begin() + static_cast<difference_type>(pos),
                                        std::move(new_leaf));
                    bm->bitmap |= bit;
                    return n;
                }
                auto clone = std::make_shared<bitmap_node>(gen, bm->depth, bm->bitmap, bm->children);
                clone->children.insert(clone->children.begin() + static_cast<difference_type>(pos),
                                       std::move(new_leaf));
                clone->bitmap |= bit;
                return clone;
            }
            node_ptr new_child = ins(bm->children[pos], h, depth + 1,
                                     std::move(k), std::move(v), gen, added, changed);
            if (new_child == bm->children[pos])
                return n;
            if (bm->gen == gen) {
                bm->children[pos] = std::move(new_child);
                return n;
            }
            auto clone = std::make_shared<bitmap_node>(gen, bm->depth, bm->bitmap, bm->children);
            clone->children[pos] = std::move(new_child);
            return clone;
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
            if (!Equal{}(k, leaf->key))
                return n;
            erased = true;
            return nullptr;
        }
        case node::kind_t::collision: {
            collision_node* col = as_collision(n);
            const auto it = std::find_if(col->entries.begin(), col->entries.end(),
                                         [&](const entry_pair& e) { return Equal{}(k, e.first); });
            if (it == col->entries.end())
                return n;
            erased = true;
            if (col->entries.size() == 1)
                return nullptr;
            if (col->entries.size() == 2) {
                const std::size_t keep = it == col->entries.begin() ? 1 : 0;
                const entry_pair& e = col->entries[keep];
                return std::make_shared<leaf_node>(gen, col->hash, e.first, e.second);
            }
            if (col->gen == gen) {
                col->entries.erase(it);
                return n;
            }
            auto clone = std::make_shared<collision_node>(gen, col->hash, col->entries);
            clone->entries.erase(clone->entries.begin() + (it - col->entries.begin()));
            return clone;
        }
        case node::kind_t::bitmap: {
            bitmap_node* bm = as_bitmap(n);
            assert(bm->depth == depth);
            const std::uint32_t bit = detail::bit_for(h, depth);
            if ((bm->bitmap & bit) == 0)
                return n;
            const std::size_t pos =
                static_cast<std::size_t>(std::popcount(bm->bitmap & (bit - 1)));
            node_ptr new_child = del(bm->children[pos], h, depth + 1, k, gen, erased);
            if (!erased)
                return n;
            if (new_child == bm->children[pos])
                return n;
            if (new_child) {
                if (bm->gen == gen) {
                    bm->children[pos] = std::move(new_child);
                    return n;
                }
                auto clone = std::make_shared<bitmap_node>(gen, bm->depth, bm->bitmap, bm->children);
                clone->children[pos] = std::move(new_child);
                return clone;
            }
            if (bm->children.size() == 1)
                return nullptr;
            if (bm->children.size() == 2) {
                const std::size_t keep = pos == 0 ? 1 : 0;
                if (bm->children[keep]->kind != node::kind_t::bitmap)
                    return bm->children[keep];
            }
            if (bm->gen == gen) {
                bm->children.erase(bm->children.begin() + static_cast<difference_type>(pos));
                bm->bitmap &= ~bit;
                return n;
            }
            auto clone = std::make_shared<bitmap_node>(gen, bm->depth, bm->bitmap, bm->children);
            clone->children.erase(clone->children.begin() + static_cast<difference_type>(pos));
            clone->bitmap &= ~bit;
            return clone;
        }
        }
        return n;
    }

public:    class const_iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using iterator_concept = std::forward_iterator_tag;
        using value_type = std::pair<const Key, Value>;
        using difference_type = std::ptrdiff_t;
        using pointer = const value_type*;
        using reference = const value_type&;

        const_iterator() noexcept = default;

        const_iterator(const const_iterator&) = default;

        const_iterator& operator=(const const_iterator& other)
        {
            if (this != &other) {
                path_ = other.path_;
                node_ = other.node_;
                entry_ = other.entry_;
                current_.reset();
            }
            return *this;
        }

        reference operator*() const
        {
            if (!current_)
                refresh();
            return *current_;
        }

        pointer operator->() const
        {
            if (!current_)
                refresh();
            return &*current_;
        }

        const_iterator& operator++()
        {
            if (node_ != nullptr && node_->kind == node::kind_t::collision) {
                const collision_node* col = as_collision(node_);
                if (entry_ + 1 < col->entries.size()) {
                    ++entry_;
                    current_.reset();
                    return *this;
                }
            }
            while (!path_.empty()) {
                frame& f = path_.back();
                const bitmap_node* bm = as_bitmap(f.parent);
                if (f.index + 1 < bm->children.size()) {
                    ++f.index;
                    node_ptr n = bm->children[f.index];
                    while (n->kind == node::kind_t::bitmap) {
                        const bitmap_node* b = as_bitmap(n);
                        path_.push_back(frame{n, 0});
                        n = b->children[0];
                    }
                    node_ = std::move(n);
                    entry_ = 0;
                    current_.reset();
                    return *this;
                }
                path_.pop_back();
            }
            node_ = nullptr;
            path_.clear();
            entry_ = 0;
            current_.reset();
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
        friend class hamt_map;

        std::vector<frame> path_;
        node_ptr node_;
        std::size_t entry_ = 0;
        mutable std::optional<value_type> current_;

        const_iterator(std::vector<frame> path, node_ptr n, std::size_t entry)
            : path_(std::move(path)), node_(std::move(n)), entry_(entry) {}

        void refresh() const
        {
            switch (node_->kind) {
            case node::kind_t::leaf: {
                const leaf_node* leaf = as_leaf(node_);
                current_.emplace(leaf->key, leaf->value);
                break;
            }
            case node::kind_t::collision: {
                const collision_node* col = as_collision(node_);
                current_.emplace(col->entries[entry_].first, col->entries[entry_].second);
                break;
            }
            case node::kind_t::bitmap:
                break;
            }
        }
    };

    hamt_map() = default;

    hamt_map(const hamt_map&) = delete;
    hamt_map& operator=(const hamt_map&) = delete;

    hamt_map(hamt_map&& other) noexcept
        : root_(std::move(other.root_)), size_(other.size_), gen_(other.gen_)
    {
        other.size_ = 0;
        other.gen_ = 0;
    }

    hamt_map& operator=(hamt_map&& other) noexcept
    {
        if (this != &other) {
            root_ = std::move(other.root_);
            size_ = other.size_;
            gen_ = other.gen_;
            other.size_ = 0;
            other.gen_ = 0;
        }
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

    [[nodiscard]] size_type size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return size_ == 0;
    }

    [[nodiscard]] hamt_map fork() noexcept
    {
        ++gen_;
        return hamt_map(root_, size_, gen_);
    }

    hamt_map& insert(Key k, Value v)
    {
        const std::uint64_t h = hash_of(k);
        if (root_ == nullptr) {
            root_ = std::make_shared<leaf_node>(gen_, h, std::move(k), std::move(v));
            ++size_;
            return *this;
        }
        bool added = false;
        bool changed = false;
        node_ptr nr = ins(root_, h, 0, std::move(k), std::move(v), gen_, added, changed);
        if (nr != root_)
            root_ = std::move(nr);
        if (changed)
            size_ += (added ? 1 : 0);
        return *this;
    }

    hamt_map& insert(const value_type& kv)
    {
        return insert(kv.first, kv.second);
    }

    hamt_map& erase(const Key& k)
    {
        if (root_ == nullptr)
            return *this;
        bool erased = false;
        node_ptr nr = del(root_, hash_of(k), 0, k, gen_, erased);
        if (nr != root_)
            root_ = std::move(nr);
        if (erased)
            --size_;
        return *this;
    }

    [[nodiscard]] const_iterator begin() const
    {
        if (root_ == nullptr)
            return const_iterator();
        std::vector<frame> path;
        node_ptr n = root_;
        while (n->kind == node::kind_t::bitmap) {
            const bitmap_node* bm = as_bitmap(n);
            path.push_back(frame{n, 0});
            n = bm->children[0];
        }
        return const_iterator(std::move(path), std::move(n), 0);
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
        std::vector<frame> path;
        node_ptr n = root_;
        for (;;) {
            switch (n->kind) {
            case node::kind_t::bitmap: {
                const bitmap_node* bm = as_bitmap(n);
                const std::uint32_t bit = detail::bit_for(h, bm->depth);
                if ((bm->bitmap & bit) == 0)
                    return const_iterator();
                const std::size_t pos =
                    static_cast<std::size_t>(std::popcount(bm->bitmap & (bit - 1)));
                path.push_back(frame{n, pos});
                n = bm->children[pos];
                break;
            }
            case node::kind_t::leaf: {
                const leaf_node* leaf = as_leaf(n);
                if (Equal{}(k, leaf->key))
                    return const_iterator(std::move(path), std::move(n), 0);
                return const_iterator();
            }
            case node::kind_t::collision: {
                const collision_node* col = as_collision(n);
                for (std::size_t i = 0; i < col->entries.size(); ++i) {
                    if (Equal{}(k, col->entries[i].first))
                        return const_iterator(std::move(path), std::move(n), i);
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
        return find_in(root_, hash_of(k), k).node != nullptr;
    }

    [[nodiscard]] size_type count(const Key& k) const
    {
        return contains(k) ? 1 : 0;
    }

    void swap(hamt_map& other) noexcept
    {
        root_.swap(other.root_);
        std::swap(size_, other.size_);
        std::swap(gen_, other.gen_);
    }

    friend void swap(hamt_map& a, hamt_map& b) noexcept
    {
        a.swap(b);
    }

    friend bool operator==(const hamt_map& a, const hamt_map& b)
        requires std::equality_comparable<Value>
    {
        if (a.size_ != b.size_)
            return false;
        auto ia = a.begin();
        auto ib = b.begin();
        const auto ae = a.end();
        for (; ia != ae; ++ia, ++ib) {
            if (!Equal{}(ia->first, ib->first) || !(ia->second == ib->second))
                return false;
        }
        return true;
    }

    friend bool operator!=(const hamt_map& a, const hamt_map& b)
        requires std::equality_comparable<Value>
    {
        return !(a == b);
    }

private:
    struct find_result
    {
        node_ptr node;
        std::size_t entry = 0;
    };

    static find_result find_in(const node_ptr& n, std::uint64_t h, const Key& k)
    {
        node_ptr cur = n;
        for (;;) {
            switch (cur->kind) {
            case node::kind_t::bitmap: {
                const bitmap_node* bm = as_bitmap(cur);
                const std::uint32_t bit = detail::bit_for(h, bm->depth);
                if ((bm->bitmap & bit) == 0)
                    return {};
                const std::size_t pos =
                    static_cast<std::size_t>(std::popcount(bm->bitmap & (bit - 1)));
                cur = bm->children[pos];
                break;
            }
            case node::kind_t::leaf: {
                const leaf_node* leaf = as_leaf(cur);
                if (Equal{}(k, leaf->key))
                    return {cur, 0};
                return {};
            }
            case node::kind_t::collision: {
                const collision_node* col = as_collision(cur);
                for (std::size_t i = 0; i < col->entries.size(); ++i) {
                    if (Equal{}(k, col->entries[i].first))
                        return {cur, i};
                }
                return {};
            }
            }
        }
    }

    node_ptr root_;
    size_type size_ = 0;
    std::uint64_t gen_ = 0;

    hamt_map(node_ptr root, size_type size, std::uint64_t gen)
        : root_(std::move(root)), size_(size), gen_(gen) {}
};

} // namespace hamt
