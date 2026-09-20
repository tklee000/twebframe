#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace TWebFrame::Internal {

// A compact map for TWebFrame's hot DOM/CSS/JavaScript paths. Values are kept
// contiguously and buckets contain only vector indices. This avoids the
// per-element node allocation and checked STL hash iterators that are
// particularly expensive in MSVC Debug builds.
//
// TWebFrame does not erase individual entries, so no tombstone state is needed.
template <typename Key, typename T, typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>>
class FastMap {
public:
    using key_type = Key;
    using mapped_type = T;
    using value_type = std::pair<Key, T>;
    using size_type = std::size_t;

    class const_iterator;

    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = FastMap::value_type;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type&;

        iterator() = default;
        reference operator*() const { return *current_; }
        pointer operator->() const { return current_; }
        iterator& operator++() { ++current_; return *this; }
        iterator operator++(int) { auto old = *this; ++*this; return old; }
        friend bool operator==(iterator left, iterator right) { return left.current_ == right.current_; }
        friend bool operator!=(iterator left, iterator right) { return !(left == right); }

    private:
        explicit iterator(pointer current) : current_(current) {}
        pointer current_ = nullptr;
        friend class FastMap;
        friend class const_iterator;
    };

    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = FastMap::value_type;
        using difference_type = std::ptrdiff_t;
        using pointer = const value_type*;
        using reference = const value_type&;

        const_iterator() = default;
        const_iterator(iterator other) : current_(other.current_) {}
        reference operator*() const { return *current_; }
        pointer operator->() const { return current_; }
        const_iterator& operator++() { ++current_; return *this; }
        const_iterator operator++(int) { auto old = *this; ++*this; return old; }
        friend bool operator==(const_iterator left, const_iterator right) { return left.current_ == right.current_; }
        friend bool operator!=(const_iterator left, const_iterator right) { return !(left == right); }

    private:
        explicit const_iterator(pointer current) : current_(current) {}
        pointer current_ = nullptr;
        friend class FastMap;
    };

    FastMap() = default;

    FastMap(std::initializer_list<value_type> values) {
        reserve(values.size());
        for (const auto& value : values) emplace(value.first, value.second);
    }

    iterator begin() { return iterator(entries_.data()); }
    iterator end() { return iterator(EndPointer()); }
    const_iterator begin() const { return const_iterator(entries_.data()); }
    const_iterator end() const { return const_iterator(EndPointer()); }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

    bool empty() const { return entries_.empty(); }
    size_type size() const { return entries_.size(); }

    void clear() {
        entries_.clear();
        std::fill(buckets_.begin(), buckets_.end(), kEmptyBucket);
    }

    void reserve(size_type count) {
        entries_.reserve(count);
        if (count <= kLinearEntryLimit && buckets_.empty()) return;
        size_type bucketCount = kInitialBucketCount;
        while (MaxEntries(bucketCount) < count) bucketCount *= 2;
        if (bucketCount > buckets_.size()) RebuildBuckets(bucketCount);
    }

    iterator find(const Key& key) {
        const auto index = FindIndex(key);
        return index == kEmptyBucket ? end() : iterator(entries_.data() + index);
    }

    const_iterator find(const Key& key) const {
        const auto index = FindIndex(key);
        return index == kEmptyBucket ? end() : const_iterator(entries_.data() + index);
    }

    size_type count(const Key& key) const { return FindIndex(key) == kEmptyBucket ? 0u : 1u; }

    size_type erase(const Key& key) {
        const auto index = FindIndex(key);
        if (index == kEmptyBucket) return 0;
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
        if (!buckets_.empty()) RebuildBuckets(buckets_.size());
        return 1;
    }

    T& operator[](const Key& key) { return FindOrAdd(key); }
    T& operator[](Key&& key) { return FindOrAdd(std::move(key)); }

    template <typename KeyArg, typename... ValueArgs>
    std::pair<iterator, bool> emplace(KeyArg&& key, ValueArgs&&... valueArgs) {
        auto index = FindIndex(key);
        if (index != kEmptyBucket) return {iterator(entries_.data() + index), false};

        EnsureInsertCapacity();
        entries_.emplace_back(std::forward<KeyArg>(key),
                              T(std::forward<ValueArgs>(valueArgs)...));
        index = entries_.size() - 1;
        if (!buckets_.empty()) PlaceIndex(index);
        return {iterator(entries_.data() + index), true};
    }

private:
    static constexpr size_type kEmptyBucket = std::numeric_limits<size_type>::max();
    static constexpr size_type kInitialBucketCount = 8;
    static constexpr size_type kLinearEntryLimit = 6;

    static size_type MaxEntries(size_type bucketCount) {
        return bucketCount * 7 / 10;
    }

    static size_type SpreadHash(size_type hash) {
        if constexpr (sizeof(size_type) == 8) {
            hash ^= hash >> 30;
            hash *= static_cast<size_type>(0xbf58476d1ce4e5b9ull);
            hash ^= hash >> 27;
            hash *= static_cast<size_type>(0x94d049bb133111ebull);
            hash ^= hash >> 31;
        } else {
            hash ^= hash >> 16;
            hash *= static_cast<size_type>(0x7feb352du);
            hash ^= hash >> 15;
            hash *= static_cast<size_type>(0x846ca68bu);
            hash ^= hash >> 16;
        }
        return hash;
    }

    value_type* EndPointer() {
        return entries_.empty() ? entries_.data() : entries_.data() + entries_.size();
    }

    const value_type* EndPointer() const {
        return entries_.empty() ? entries_.data() : entries_.data() + entries_.size();
    }

    size_type FindIndex(const Key& key) const {
        if (buckets_.empty()) {
            for (size_type index = 0; index < entries_.size(); ++index)
                if (equal_(entries_[index].first, key)) return index;
            return kEmptyBucket;
        }
        const size_type mask = buckets_.size() - 1;
        size_type bucket = SpreadHash(hash_(key)) & mask;
        for (;;) {
            const size_type index = buckets_[bucket];
            if (index == kEmptyBucket) return kEmptyBucket;
            if (equal_(entries_[index].first, key)) return index;
            bucket = (bucket + 1) & mask;
        }
    }

    template <typename KeyArg>
    T& FindOrAdd(KeyArg&& key) {
        auto index = FindIndex(key);
        if (index != kEmptyBucket) return entries_[index].second;

        EnsureInsertCapacity();
        entries_.emplace_back(std::forward<KeyArg>(key), T{});
        index = entries_.size() - 1;
        if (!buckets_.empty()) PlaceIndex(index);
        return entries_[index].second;
    }

    void EnsureInsertCapacity() {
        if (buckets_.empty()) {
            if (entries_.size() < kLinearEntryLimit) {
                if (entries_.size() == entries_.capacity())
                    entries_.reserve(entries_.empty() ? 4 : entries_.capacity() * 2);
                return;
            }
            RebuildBuckets(kInitialBucketCount * 2);
        } else if (entries_.size() + 1 > MaxEntries(buckets_.size())) {
            RebuildBuckets(buckets_.size() * 2);
        }
    }

    void RebuildBuckets(size_type count) {
        entries_.reserve(MaxEntries(count));
        buckets_.assign(count, kEmptyBucket);
        for (size_type index = 0; index < entries_.size(); ++index) PlaceIndex(index);
    }

    void PlaceIndex(size_type index) {
        const size_type mask = buckets_.size() - 1;
        size_type bucket = SpreadHash(hash_(entries_[index].first)) & mask;
        while (buckets_[bucket] != kEmptyBucket) bucket = (bucket + 1) & mask;
        buckets_[bucket] = index;
    }

    std::vector<value_type> entries_;
    std::vector<size_type> buckets_;
    Hash hash_;
    Equal equal_;
};

} // namespace TWebFrame::Internal
