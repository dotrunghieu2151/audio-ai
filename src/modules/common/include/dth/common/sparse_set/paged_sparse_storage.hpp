#pragma once

#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "dth/common/concepts/power_of_two.hpp"
#include "dth/common/sparse_set/storage_policy.hpp"
namespace dth::common {
using namespace concepts;

template <SparseSetKey Key, std::size_t PageSize = 16>
    requires concepts::PowerOfTwo<PageSize>
class PagedSparseStorage
{
    using index_type = std::size_t;
    using Page       = std::vector<index_type>;

  public:
    void ensure(Key key)
    {
        const auto        k          = static_cast<std::size_t>(key);
        const std::size_t page_index = get_page(k);
        if (page_index >= _pages.size()) {
            _pages.resize(page_index + 1);
        }

        if (!_pages[page_index]) {
            _pages[page_index] = std::make_unique<Page>(PageSize, invalid_index);
        }
    }

    [[nodiscard]]
    std::optional<index_type> get(Key key) const noexcept
    {
        const std::size_t page_index = get_page(key);
        if (page_index >= _pages.size() || !_pages[page_index]) {
            return std::nullopt;
        }

        const std::size_t offset = get_offset(key);
        const Page&       page   = *_pages[page_index];
        return page[offset] != invalid_index ? std::make_optional<index_type>(page[offset])
                                             : std::nullopt;
    }

    void set(Key key, index_type index)
    {
        ensure(key);
        (*_pages[get_page(key)])[get_offset(key)] = index;
    }

    void erase(Key key)
    {
        const std::size_t page = get_page(key);
        if (page < _pages.size() && _pages[page]) {
            (*_pages[page])[get_offset(key)] = invalid_index;
        }
    }
    void clear()
    {
        for (auto& pagePtr : _pages) {
            if (!pagePtr) {
                continue;
            }
            // fast reset all element to all 1 bits (maximum value) with memset
            std::memset(pagePtr->data(), 0xFF, pagePtr->size() * sizeof(index_type));
        }
    }

  private:
    // used to indicate absent index or index has been erased
    static constexpr index_type  invalid_index = std::numeric_limits<index_type>::max();
    static constexpr std::size_t page_shift    = std::countr_zero(PageSize);

    [[nodiscard]] static constexpr std::size_t get_page(std::size_t index) noexcept
    {
        return index >> page_shift;
    }

    [[nodiscard]] static constexpr std::size_t get_offset(std::size_t index) noexcept
    {
        return index & (PageSize - 1);
    }
    std::vector<std::unique_ptr<Page>> _pages;
};
}  // namespace dth::common
