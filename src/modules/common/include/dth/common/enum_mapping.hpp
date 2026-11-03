#pragma once

#include <array>
#include <cstddef>

namespace dth::common {

template <typename TFrom, typename TTo, auto... Mappings>
struct EnumMap
{
  private:
    static consteval auto make_table()
    {
        constexpr auto max_value = [] {
            int result = -1;
            ((result = result < static_cast<int>(Mappings.first) ? static_cast<int>(Mappings.first)
                                                                 : result),
             ...);
            return result;
        }();

        std::array<TTo, max_value + 1> table{};

        ((table[static_cast<std::size_t>(Mappings.first)] = Mappings.second), ...);

        return table;
    }

    static consteval bool validate()
    {
        constexpr auto entries = std::array{Mappings...};

        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto value = static_cast<int>(entries[i].first);

            if (value < 0) {
                return false;
            }

            for (std::size_t j = i + 1; j < entries.size(); ++j) {
                if (entries[i].first == entries[j].first) {
                    return false;
                }
            }
        }

        return true;
    }

    static_assert(validate(), "Invalid or duplicate enum mapping");

    inline static constexpr auto table = make_table();

  public:
    [[nodiscard]]
    static constexpr TTo map(TFrom value) noexcept
    {
        const auto index = static_cast<int>(value);

        return table[static_cast<std::size_t>(index)];
    }
};

}  // namespace dth::common
