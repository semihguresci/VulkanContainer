#include "Container/renderer/bim/BimMetadataIndex.h"

#include <gtest/gtest.h>

#include <limits>

namespace {

using container::renderer::BimElementMetadata;
using container::renderer::BimMetadataIndex;

TEST(BimMetadataIndexTests, IndexesGuidAndSourceId) {
  BimMetadataIndex index;
  index.reserve(3u);

  BimElementMetadata first{};
  first.objectIndex = 2u;
  first.guid = "guid-a";
  first.sourceId = "source-a";
  index.index(first);

  BimElementMetadata second{};
  second.objectIndex = 5u;
  second.guid = "guid-a";
  second.sourceId = "source-b";
  index.index(second);

  const auto guidMatches = index.objectIndicesForGuid("guid-a");
  ASSERT_EQ(guidMatches.size(), 2u);
  EXPECT_EQ(guidMatches[0], 2u);
  EXPECT_EQ(guidMatches[1], 5u);

  const auto sourceMatches = index.objectIndicesForSourceId("source-b");
  ASSERT_EQ(sourceMatches.size(), 1u);
  EXPECT_EQ(sourceMatches[0], 5u);
}

TEST(BimMetadataIndexTests, IgnoresInvalidAndEmptyIdentities) {
  BimMetadataIndex index;

  BimElementMetadata invalid{};
  invalid.objectIndex = std::numeric_limits<uint32_t>::max();
  invalid.guid = "ignored";
  invalid.sourceId = "ignored-source";
  index.index(invalid);

  BimElementMetadata empty{};
  empty.objectIndex = 1u;
  index.index(empty);

  EXPECT_TRUE(index.objectIndicesForGuid("ignored").empty());
  EXPECT_TRUE(index.objectIndicesForSourceId("ignored-source").empty());
  EXPECT_TRUE(index.objectIndicesForGuid("").empty());
  EXPECT_TRUE(index.objectIndicesForSourceId("").empty());
}

} // namespace
