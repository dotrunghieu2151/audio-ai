
#include <gtest/gtest.h>

#include <cstdint>

#include "dth/common/sparse_set/flat_sparse_storage.hpp"
#include "dth/common/sparse_set/paged_sparse_storage.hpp"

namespace dth::common::test {

using Key = std::uint32_t;

// Run the shared tests against both implementations.
using StorageTypes = ::testing::Types<FlatSparseStorage<Key>, PagedSparseStorage<Key, 4>>;

template <typename Storage>
class SparseStorageTest : public ::testing::Test
{
  protected:
    Storage storage;
};

TYPED_TEST_SUITE(SparseStorageTest, StorageTypes);

TYPED_TEST(SparseStorageTest, UnseenKeyReturnsInvalidIndex)
{
    EXPECT_TRUE(!this->storage.get(0).has_value());
    EXPECT_TRUE(!this->storage.get(42).has_value());
    EXPECT_TRUE(!this->storage.get(100).has_value());
}

TYPED_TEST(SparseStorageTest, SetAndGetMapping)
{
    this->storage.set(42, 7);

    EXPECT_EQ(this->storage.get(42).value(), 7);
}

TYPED_TEST(SparseStorageTest, SetSameKeyUpdatesMapping)
{
    this->storage.set(42, 7);
    this->storage.set(42, 12);

    EXPECT_EQ(this->storage.get(42).value(), 12);
}

TYPED_TEST(SparseStorageTest, DifferentKeysHaveIndependentMappings)
{
    this->storage.set(1, 10);
    this->storage.set(2, 20);
    this->storage.set(3, 30);

    EXPECT_EQ(this->storage.get(1).value(), 10);
    EXPECT_EQ(this->storage.get(2).value(), 20);
    EXPECT_EQ(this->storage.get(3).value(), 30);
}

TYPED_TEST(SparseStorageTest, ClearInvalidatesMapping)
{
    this->storage.set(42, 7);

    this->storage.erase(42);

    EXPECT_TRUE(!this->storage.get(42).has_value());
}

TYPED_TEST(SparseStorageTest, ClearDoesNotAffectOtherKeys)
{
    this->storage.set(1, 10);
    this->storage.set(2, 20);
    this->storage.set(3, 30);

    this->storage.erase(2);

    EXPECT_EQ(this->storage.get(1).value(), 10);
    EXPECT_TRUE(!this->storage.get(2).has_value());
    EXPECT_EQ(this->storage.get(3).value(), 30);
}

TYPED_TEST(SparseStorageTest, ClearingUnseenKeyIsSafe)
{
    EXPECT_NO_THROW(this->storage.erase(42));

    EXPECT_TRUE(!this->storage.get(42).has_value());
}

TYPED_TEST(SparseStorageTest, EnsureDoesNotCreateMapping)
{
    this->storage.ensure(42);

    EXPECT_TRUE(!this->storage.get(42).has_value());
}

TYPED_TEST(SparseStorageTest, ClearThenSetWorks)
{
    this->storage.set(42, 7);
    this->storage.erase(42);

    this->storage.set(42, 15);

    EXPECT_EQ(this->storage.get(42).value(), 15);
}

TYPED_TEST(SparseStorageTest, RepeatedClearIsSafe)
{
    this->storage.set(42, 7);

    this->storage.erase(42);
    this->storage.erase(42);

    EXPECT_TRUE(!this->storage.get(42).has_value());
}

}  // namespace dth::common::test
