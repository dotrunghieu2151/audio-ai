
#include "dth/common/sparse_set/paged_sparse_storage.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace dth::common::test {

using Key     = std::uint32_t;
using Storage = dth::common::PagedSparseStorage<Key, 4>;

TEST(PagedSparseStorageTest, HandlesFirstAndLastKeyOfPage)
{
    Storage storage;

    storage.set(0, 10);
    storage.set(3, 13);

    EXPECT_EQ(storage.get(0).value(), 10);
    EXPECT_EQ(storage.get(3).value(), 13);
}

TEST(PagedSparseStorageTest, HandlesKeyAtPageBoundary)
{
    Storage storage;

    // Page 0: keys 0..3
    // Page 1: keys 4..7
    storage.set(3, 30);
    storage.set(4, 40);

    EXPECT_EQ(storage.get(3).value(), 30);
    EXPECT_EQ(storage.get(4).value(), 40);
}

TEST(PagedSparseStorageTest, UnallocatedPageReturnsInvalidIndex)
{
    Storage storage;

    // Allocate page 0, but not page 1.
    storage.set(0, 10);

    EXPECT_TRUE(!storage.get(4).has_value());
    EXPECT_TRUE(!storage.get(7).has_value());
}

TEST(PagedSparseStorageTest, ClearingOnePageDoesNotAffectAnother)
{
    Storage storage;

    storage.set(1, 11);
    storage.set(5, 55);

    storage.erase(1);

    EXPECT_TRUE(!storage.get(1).has_value());
    EXPECT_EQ(storage.get(5).value(), 55);
}

TEST(PagedSparseStorageTest, SupportsNonContiguousPages)
{
    Storage storage;

    // Pages 0 and 3 are allocated; pages 1 and 2 remain unallocated.
    storage.set(0, 100);
    storage.set(12, 200);

    EXPECT_EQ(storage.get(0).value(), 100);
    EXPECT_EQ(storage.get(12).value(), 200);

    EXPECT_TRUE(!storage.get(4).has_value());
    EXPECT_TRUE(!storage.get(8).has_value());
}

TEST(PagedSparseStorageTest, EnsureAllocatesPageWithoutSettingMapping)
{
    Storage storage;

    storage.ensure(8);

    EXPECT_TRUE(!storage.get(8).has_value());

    storage.set(8, 80);

    EXPECT_EQ(storage.get(8).value(), 80);
}

}  // namespace dth::common::test
