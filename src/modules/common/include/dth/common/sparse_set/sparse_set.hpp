#pragma once
#include <concepts>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

#include "dth/common/sparse_set/flat_sparse_storage.hpp"
#include "dth/common/sparse_set/storage_policy.hpp"

namespace dth::common {

template <concepts::SparseSetKey              TKey, typename TValue,
          concepts::SparseStoragePolicy<TKey> SparseStoragePolicy = FlatSparseStorage<TKey>>
class SparseSet final
{
    static_assert(!std::same_as<TKey, bool>, "SparseSet key must be an unsigned integer, not bool");

    static_assert(std::is_nothrow_move_constructible_v<TValue>,
                  "SparseSet values must be nothrow move constructible");

    static_assert(std::is_nothrow_move_assignable_v<TValue>,
                  "SparseSet values must be nothrow move assignable");

  public:
    SparseSet()                 = default;
    ~SparseSet()                = default;
    SparseSet(const SparseSet&) = delete;
    SparseSet(SparseSet&&)      = default;

    SparseSet& operator=(const SparseSet&) = delete;
    SparseSet& operator=(SparseSet&&)      = default;

    [[nodiscard]]
    bool empty() const noexcept
    {
        return _dense_array.empty();
    }

    [[nodiscard]]
    std::size_t size() const noexcept
    {
        return _dense_array.size();
    }

    [[nodiscard]]
    bool contains(TKey key) const noexcept
    {
        const auto index = _sparse_storage.get(key);

        return index.has_value();
    }

    TValue& at(TKey key)
    {
        const auto index = _sparse_storage.get(key);
        if (!index.has_value()) {
            throw std::out_of_range("key not found inside sparseset");
        }
        return _dense_array[index.value()];
    }
    const TValue& at(TKey key) const
    {
        const auto index = _sparse_storage.get(key);
        if (!index.has_value()) {
            throw std::out_of_range("key not found inside sparset");
        }
        return _dense_array[index.value()];
    }

    [[nodiscard]]
    TValue* find(TKey key) noexcept
    {
        if (!contains(key)) {
            return nullptr;
        }
        return &_dense_array[_sparse_storage.get(key).value()];
    }

    [[nodiscard]]
    const TValue* find(TKey key) const noexcept
    {
        if (!contains(key)) {
            return nullptr;
        }
        return &_dense_array[_sparse_storage.get(key).value()];
    }

    template <typename... Args>
    std::pair<TValue*, bool> try_emplace(TKey key, Args&&... args)
    {
        if (auto existingValuePtr = find(key)) {
            return std::make_pair(existingValuePtr, false);
        }
        const std::size_t index = _dense_array.size();
        TValue            value(std::forward<Args>(args)...);
        _key_array.push_back(key);
        try {
            _dense_array.push_back(std::move(value));
        } catch (...) {
            _key_array.pop_back();
            throw;
        }

        try {
            _sparse_storage.set(key, index);
        } catch (...) {
            _dense_array.pop_back();
            _key_array.pop_back();
            throw;
        }

        return std::make_pair(&_dense_array.back(), true);
    }

    bool insert(TKey key, const TValue& value) { return try_emplace(key, value).second; }

    bool insert(TKey key, TValue&& value) { return try_emplace(key, std::move(value)).second; }

    bool erase(TKey key) noexcept
    {
        if (!contains(key)) {
            return false;
        }
        const std::size_t index = _sparse_storage.get(key).value();
        const std::size_t last  = _dense_array.size() - 1;

        if (index != last) {
            // Move the final element to the removed slot
            std::swap(_key_array[index], _key_array[last]);
            std::swap(_dense_array[index], _dense_array[last]);

            // Update the moved key's sparse mapping
            _sparse_storage.set(_key_array[index], index);
        }

        _sparse_storage.erase(key);
        _key_array.pop_back();
        _dense_array.pop_back();
        return true;
    }

    void reserve(std::size_t count)
    {
        _sparse_storage.ensure(count);
        _dense_array.reserve(count);
        _key_array.reserve(count);
    }

    void clear() noexcept
    {
        _dense_array.clear();
        _key_array.clear();
        _sparse_storage.clear();
    }

    [[nodiscard]]
    const std::vector<TValue>& values() noexcept
    {
        return _dense_array;
    }

    [[nodiscard]]
    const std::vector<TKey>& keys() noexcept
    {
        return _key_array;
    }

  private:
    std::vector<TValue> _dense_array;
    std::vector<TKey>   _key_array;
    SparseStoragePolicy _sparse_storage;
};
}  // namespace dth::common
