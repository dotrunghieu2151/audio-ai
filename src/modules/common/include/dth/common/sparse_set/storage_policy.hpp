#pragma once

#include <concepts>
#include <cstddef>
#include <optional>

namespace dth::common::concepts {

using index_type = std::size_t;

template <typename Key>
concept SparseSetKey = std::unsigned_integral<Key> && (sizeof(Key) <= sizeof(index_type));

template <typename Storage, typename Key>
concept SparseStoragePolicy =
    SparseSetKey<Key> &&
    requires(Storage storage, const Storage const_storage, Key key, index_type index) {
        // Specifies interfaces that a valid StoragePolicy must enforce
        // Ensure storage exists for this key
        { storage.ensure(key) } -> std::same_as<void>;

        // Retrive the dense index, or invalid_index
        { const_storage.get(key) } -> std::same_as<std::optional<index_type>>;
        // Map key to index
        { storage.set(key, index) } -> std::same_as<void>;

        // Remove key to index mapping
        { storage.erase(key) } -> std::same_as<void>;

        { storage.clear() } -> std::same_as<void>;
    };
}  // namespace dth::common::concepts
