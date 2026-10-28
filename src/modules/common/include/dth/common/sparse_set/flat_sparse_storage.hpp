#pragma once

#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

#include "dth/common/sparse_set/storage_policy.hpp"
namespace dth::common {
using namespace concepts;

template <SparseSetKey Key>
class FlatSparseStorage
{
    using index_type = std::size_t;

  public:
    void ensure(Key key)
    {
        const auto k = static_cast<std::size_t>(key);
        if (k == invalid_index) {
            throw std::length_error("SparseSetKey cannot be represented as it overflows");
        }

        if (k >= _sparse_array.size()) {
            const std::size_t required_size = k + 1;
            // Check the vector's maximum supported size.
            if (required_size > _sparse_array.max_size()) {
                throw std::length_error("SparseSet capacity exceeded");
            }

            // initialize spare space with invalid_index
            _sparse_array.resize(required_size, invalid_index);
        }
    }

    [[nodiscard]]
    std::optional<index_type> get(Key key) const noexcept
    {
        if (key >= _sparse_array.size()) {
            return std::nullopt;
        }

        return _sparse_array[key] != invalid_index ? std::make_optional(_sparse_array[key])
                                                   : std::nullopt;
    }

    void set(Key key, index_type index)
    {
        ensure(key);
        _sparse_array[key] = index;
    }

    void erase(Key key)
    {
        if (key < _sparse_array.size()) {
            _sparse_array[key] = invalid_index;
        }
    }

    void clear()
    {
        // fast reset all element to all 1 bits (maximum value) with memset
        std::memset(_sparse_array.data(), 0xFF, _sparse_array.size() * sizeof(index_type));
    }

  private:
    // used to indicate absent index or index has been erased
    static constexpr index_type invalid_index = std::numeric_limits<index_type>::max();
    std::vector<index_type>     _sparse_array;
};
}  // namespace dth::common
