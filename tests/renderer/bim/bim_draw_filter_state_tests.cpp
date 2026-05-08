#include "Container/renderer/bim/BimDrawFilterState.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using container::renderer::BimDrawFilter;
using container::renderer::BimDrawFilterState;
using container::renderer::BimDrawFilterStateInputs;
using container::renderer::BimElementMetadata;
using container::renderer::DrawCommand;

[[nodiscard]] BimDrawFilterStateInputs
makeInputs(uint64_t revision, const std::vector<BimElementMetadata> &metadata,
           const std::vector<DrawCommand> &opaqueSingleSided) {
  return BimDrawFilterStateInputs{
      .revision = revision,
      .objectCount = metadata.size(),
      .metadata = {metadata.data(), metadata.size()},
      .opaqueSingleSidedDrawCommands = &opaqueSingleSided,
  };
}

TEST(BimDrawFilterStateTests, FiltersAndMergesInstancedSurfaceCommands) {
  std::vector<BimElementMetadata> metadata(3u);
  metadata[0].objectIndex = 0u;
  metadata[0].type = "Wall";
  metadata[1].objectIndex = 1u;
  metadata[1].type = "Wall";
  metadata[2].objectIndex = 2u;
  metadata[2].type = "Door";

  const std::vector<DrawCommand> opaqueSingleSided{
      DrawCommand{.objectIndex = 0u,
                  .firstIndex = 12u,
                  .indexCount = 6u,
                  .instanceCount = 3u},
  };

  BimDrawFilter filter{};
  filter.typeFilterEnabled = true;
  filter.type = "Wall";

  BimDrawFilterState state;
  const auto inputs = makeInputs(7u, metadata, opaqueSingleSided);
  const auto &lists = state.filteredDrawLists(filter, inputs);

  ASSERT_EQ(lists.opaqueSingleSidedDrawCommands.size(), 1u);
  EXPECT_EQ(lists.opaqueSingleSidedDrawCommands[0].objectIndex, 0u);
  EXPECT_EQ(lists.opaqueSingleSidedDrawCommands[0].firstIndex, 12u);
  EXPECT_EQ(lists.opaqueSingleSidedDrawCommands[0].indexCount, 6u);
  EXPECT_EQ(lists.opaqueSingleSidedDrawCommands[0].instanceCount, 2u);
  EXPECT_TRUE(state.objectMatchesFilter(1u, filter, inputs));
  EXPECT_FALSE(state.objectMatchesFilter(2u, filter, inputs));
}

TEST(BimDrawFilterStateTests, SelectionFiltersUseProductIdentity) {
  std::vector<BimElementMetadata> metadata(3u);
  metadata[0].objectIndex = 0u;
  metadata[0].guid = "shared-guid";
  metadata[1].objectIndex = 1u;
  metadata[1].guid = "shared-guid";
  metadata[2].objectIndex = 2u;
  metadata[2].guid = "other-guid";
  const std::vector<DrawCommand> opaqueSingleSided;

  BimDrawFilter isolate{};
  isolate.isolateSelection = true;
  isolate.selectedObjectIndex = 0u;

  BimDrawFilterState state;
  const auto inputs = makeInputs(1u, metadata, opaqueSingleSided);
  EXPECT_TRUE(state.objectMatchesFilter(0u, isolate, inputs));
  EXPECT_TRUE(state.objectMatchesFilter(1u, isolate, inputs));
  EXPECT_FALSE(state.objectMatchesFilter(2u, isolate, inputs));

  BimDrawFilter hide{};
  hide.hideSelection = true;
  hide.selectedObjectIndex = 0u;
  EXPECT_FALSE(state.objectMatchesFilter(0u, hide, inputs));
  EXPECT_FALSE(state.objectMatchesFilter(1u, hide, inputs));
  EXPECT_TRUE(state.objectMatchesFilter(2u, hide, inputs));
}

TEST(BimDrawFilterStateTests, CachedListsRefreshOnlyWhenRevisionChanges) {
  std::vector<BimElementMetadata> metadata(2u);
  metadata[0].objectIndex = 0u;
  metadata[0].type = "Wall";
  metadata[1].objectIndex = 1u;
  metadata[1].type = "Wall";
  std::vector<DrawCommand> opaqueSingleSided{
      DrawCommand{.objectIndex = 0u,
                  .firstIndex = 4u,
                  .indexCount = 3u,
                  .instanceCount = 1u},
  };

  BimDrawFilter filter{};
  filter.typeFilterEnabled = true;
  filter.type = "Wall";

  BimDrawFilterState state;
  auto inputs = makeInputs(1u, metadata, opaqueSingleSided);
  size_t filteredCount = state.filteredDrawLists(filter, inputs)
                             .opaqueSingleSidedDrawCommands.size();
  ASSERT_EQ(filteredCount, 1u);

  opaqueSingleSided.push_back(DrawCommand{.objectIndex = 1u,
                                          .firstIndex = 8u,
                                          .indexCount = 3u,
                                          .instanceCount = 1u});
  filteredCount = state.filteredDrawLists(filter, inputs)
                      .opaqueSingleSidedDrawCommands.size();
  EXPECT_EQ(filteredCount, 1u);

  inputs.revision = 2u;
  filteredCount = state.filteredDrawLists(filter, inputs)
                      .opaqueSingleSidedDrawCommands.size();
  EXPECT_EQ(filteredCount, 2u);
}

} // namespace
