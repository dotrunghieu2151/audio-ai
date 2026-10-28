
#include "dth/common/sparse_set/sparse_set.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "dth/common/sparse_set/flat_sparse_storage.hpp"
#include "dth/common/sparse_set/paged_sparse_storage.hpp"

namespace dth::common::test {

using Key   = std::uint32_t;
using Value = std::string;

using StorageTypes = ::testing::Types<FlatSparseStorage<Key>, PagedSparseStorage<Key, 4> >;

template <typename Storage>
class SparseSetTest : public ::testing::Test
{
  protected:
    using Set = SparseSet<Key, Value, Storage>;

    Set set;

    // Check the public dense-storage invariants.
    void expect_consistent()
    {
        ASSERT_EQ(set.keys().size(), set.values().size());
        ASSERT_EQ(set.size(), set.keys().size());

        for (std::size_t i = 0; i < set.size(); ++i) {
            const auto  key   = set.keys()[i];
            const auto* value = set.find(key);

            ASSERT_NE(value, nullptr);
            EXPECT_EQ(*value, set.values()[i]);
        }
    }
};

TYPED_TEST_SUITE(SparseSetTest, StorageTypes);

// ------------------------------------------------------------
// Basic state
// ------------------------------------------------------------

TYPED_TEST(SparseSetTest, InitiallyEmpty)
{
    EXPECT_TRUE(this->set.empty());
    EXPECT_EQ(this->set.size(), 0U);
    EXPECT_TRUE(this->set.keys().empty());
    EXPECT_TRUE(this->set.values().empty());
}

TYPED_TEST(SparseSetTest, MissingKeyIsNotContained)
{
    EXPECT_FALSE(this->set.contains(42));
    EXPECT_EQ(this->set.find(42), nullptr);
}

TYPED_TEST(SparseSetTest, InsertOneElement)
{
    auto [value, inserted] = this->set.try_emplace(42, "hello");

    ASSERT_TRUE(inserted);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, "hello");

    EXPECT_FALSE(this->set.empty());
    EXPECT_EQ(this->set.size(), 1U);
    EXPECT_TRUE(this->set.contains(42));
    EXPECT_EQ(this->set.find(42), value);

    this->expect_consistent();
}

TYPED_TEST(SparseSetTest, InsertMultipleElements)
{
    EXPECT_TRUE(this->set.insert(10, Value{"ten"}));
    EXPECT_TRUE(this->set.insert(20, Value{"twenty"}));
    EXPECT_TRUE(this->set.insert(30, Value{"thirty"}));

    EXPECT_EQ(this->set.size(), 3U);
    EXPECT_EQ(*this->set.find(10), "ten");
    EXPECT_EQ(*this->set.find(20), "twenty");
    EXPECT_EQ(*this->set.find(30), "thirty");

    this->expect_consistent();
}

// ------------------------------------------------------------
// try_emplace
// ------------------------------------------------------------

TYPED_TEST(SparseSetTest, TryEmplaceReturnsExistingValueForDuplicateKey)
{
    auto [first, first_inserted] = this->set.try_emplace(42, "original");

    auto [second, second_inserted] = this->set.try_emplace(42, "replacement");

    ASSERT_TRUE(first_inserted);
    ASSERT_FALSE(second_inserted);
    EXPECT_EQ(second, first);
    EXPECT_EQ(*second, "original");
    EXPECT_EQ(this->set.size(), 1U);

    this->expect_consistent();
}

TYPED_TEST(SparseSetTest, TryEmplaceForwardsConstructorArguments)
{
    // std::string(count, character)
    auto [value, inserted] = this->set.try_emplace(7, 5, 'x');

    ASSERT_TRUE(inserted);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, "xxxxx");
}

// ------------------------------------------------------------
// insert overloads
// ------------------------------------------------------------

TYPED_TEST(SparseSetTest, InsertLvalueCopiesValue)
{
    const Value original{"hello"};

    EXPECT_TRUE(this->set.insert(1, original));

    EXPECT_EQ(*this->set.find(1), "hello");
    EXPECT_EQ(original, "hello");
}

TYPED_TEST(SparseSetTest, InsertRvalueMovesValue)
{
    Value original{"hello"};

    EXPECT_TRUE(this->set.insert(1, std::move(original)));

    EXPECT_EQ(*this->set.find(1), "hello");
}

TYPED_TEST(SparseSetTest, InsertDuplicateDoesNotOverwriteExistingValue)
{
    EXPECT_TRUE(this->set.insert(1, Value{"original"}));
    EXPECT_FALSE(this->set.insert(1, Value{"replacement"}));

    EXPECT_EQ(this->set.size(), 1U);
    EXPECT_EQ(*this->set.find(1), "original");
}

// ------------------------------------------------------------
// Lookup and access
// ------------------------------------------------------------

TYPED_TEST(SparseSetTest, FindReturnsNullForMissingKey) { EXPECT_EQ(this->set.find(99), nullptr); }

TYPED_TEST(SparseSetTest, FindReturnsMutableValue)
{
    this->set.insert(1, Value{"before"});

    auto* value = this->set.find(1);
    ASSERT_NE(value, nullptr);

    *value = "after";

    EXPECT_EQ(*this->set.find(1), "after");
}

TYPED_TEST(SparseSetTest, ConstFindReturnsConstValue)
{
    this->set.insert(1, Value{"hello"});

    const auto& const_set = this->set;
    const auto* value     = const_set.find(1);

    static_assert(std::is_same_v<decltype(value), const Value*>);

    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, "hello");
}

TYPED_TEST(SparseSetTest, AtReturnsValueForExistingKey)
{
    this->set.insert(1, Value{"hello"});

    EXPECT_EQ(this->set.at(1), "hello");
    this->set.at(1) = "updated";

    EXPECT_EQ(*this->set.find(1), "updated");
}

TYPED_TEST(SparseSetTest, AtThrowsForMissingKey)
{
    EXPECT_THROW(this->set.at(42), std::out_of_range);
}

TYPED_TEST(SparseSetTest, ConstAtReturnsConstValue)
{
    this->set.insert(1, Value{"hello"});

    const auto& const_set = this->set;
    const auto& value     = const_set.at(1);

    static_assert(std::is_same_v<decltype(value), const Value&>);

    EXPECT_EQ(value, "hello");
}

// ------------------------------------------------------------
// Erasure
// ------------------------------------------------------------

TYPED_TEST(SparseSetTest, EraseExistingKey)
{
    this->set.insert(1, Value{"one"});
    this->set.insert(2, Value{"two"});

    EXPECT_TRUE(this->set.erase(1));

    EXPECT_EQ(this->set.size(), 1U);
    EXPECT_FALSE(this->set.contains(1));
    EXPECT_EQ(this->set.find(1), nullptr);
    EXPECT_TRUE(this->set.contains(2));
    EXPECT_EQ(*this->set.find(2), "two");

    this->expect_consistent();
}

TYPED_TEST(SparseSetTest, EraseMissingKeyReturnsFalse)
{
    this->set.insert(1, Value{"one"});

    EXPECT_FALSE(this->set.erase(99));
    EXPECT_EQ(this->set.size(), 1U);

    this->expect_consistent();
}

TYPED_TEST(SparseSetTest, EraseLastElement)
{
    this->set.insert(1, Value{"one"});

    EXPECT_TRUE(this->set.erase(1));

    EXPECT_TRUE(this->set.empty());
    EXPECT_FALSE(this->set.contains(1));

    this->expect_consistent();
}

TYPED_TEST(SparseSetTest, EraseMiddleElementPreservesOtherMappings)
{
    this->set.insert(10, Value{"ten"});
    this->set.insert(20, Value{"twenty"});
    this->set.insert(30, Value{"thirty"});

    // Removing a non-final dense element should move the final
    // element into its slot and update that element's mapping.
    EXPECT_TRUE(this->set.erase(20));

    EXPECT_EQ(this->set.size(), 2U);
    EXPECT_FALSE(this->set.contains(20));

    EXPECT_EQ(*this->set.find(10), "ten");
    EXPECT_EQ(*this->set.find(30), "thirty");

    this->expect_consistent();
}

TYPED_TEST(SparseSetTest, EraseThenReinsertSameKey)
{
    this->set.insert(42, Value{"first"});

    ASSERT_TRUE(this->set.erase(42));
    EXPECT_TRUE(this->set.insert(42, Value{"second"}));

    ASSERT_NE(this->set.find(42), nullptr);
    EXPECT_EQ(*this->set.find(42), "second");

    this->expect_consistent();
}

// ------------------------------------------------------------
// Reserve and clear
// ------------------------------------------------------------

TYPED_TEST(SparseSetTest, ReserveDoesNotChangeLogicalContents)
{
    this->set.insert(1, Value{"one"});
    this->set.insert(2, Value{"two"});

    this->set.reserve(100);

    EXPECT_EQ(this->set.size(), 2U);
    EXPECT_EQ(*this->set.find(1), "one");
    EXPECT_EQ(*this->set.find(2), "two");

    this->expect_consistent();
}

TYPED_TEST(SparseSetTest, ClearRemovesAllElements)
{
    this->set.insert(1, Value{"one"});
    this->set.insert(2, Value{"two"});
    this->set.insert(3, Value{"three"});

    this->set.clear();

    EXPECT_TRUE(this->set.empty());
    EXPECT_EQ(this->set.size(), 0U);
    EXPECT_FALSE(this->set.contains(1));
    EXPECT_FALSE(this->set.contains(2));
    EXPECT_FALSE(this->set.contains(3));

    EXPECT_TRUE(this->set.keys().empty());
    EXPECT_TRUE(this->set.values().empty());

    this->expect_consistent();
}

TYPED_TEST(SparseSetTest, ClearAllowsSubsequentInsertions)
{
    this->set.insert(1, Value{"before"});
    this->set.clear();

    EXPECT_TRUE(this->set.insert(1, Value{"after"}));

    EXPECT_EQ(this->set.size(), 1U);
    EXPECT_EQ(*this->set.find(1), "after");

    this->expect_consistent();
}

// ------------------------------------------------------------
// Dense-array invariants
// ------------------------------------------------------------

TYPED_TEST(SparseSetTest, KeysAndValuesRemainAlignedAfterErasure)
{
    this->set.insert(100, Value{"A"});
    this->set.insert(200, Value{"B"});
    this->set.insert(300, Value{"C"});
    this->set.insert(400, Value{"D"});

    this->set.erase(200);
    this->set.erase(400);

    this->expect_consistent();
}

TYPED_TEST(SparseSetTest, SupportsNonContiguousKeys)
{
    this->set.insert(1, Value{"one"});
    this->set.insert(1000, Value{"thousand"});
    this->set.insert(1'000'000, Value{"million"});

    EXPECT_EQ(this->set.size(), 3U);
    EXPECT_EQ(*this->set.find(1), "one");
    EXPECT_EQ(*this->set.find(1000), "thousand");
    EXPECT_EQ(*this->set.find(1'000'000), "million");

    this->expect_consistent();
}

}  // namespace dth::common::test
