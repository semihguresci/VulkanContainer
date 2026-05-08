#include "Container/utility/BcfViewpoint.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

container::ui::ViewpointSnapshotState sampleSnapshot() {
  container::ui::ViewpointSnapshotState snapshot{};
  snapshot.label = "Door review";
  snapshot.camera.position = {1.0f, 2.0f, 3.0f};
  snapshot.camera.rotationDegrees = {-15.0f, 45.0f, 0.0f};
  snapshot.camera.scale = {1.0f, 1.0f, 1.0f};
  snapshot.selectedMeshNode = 12u;
  snapshot.selectedBimObjectIndex = 34u;
  snapshot.selectedBimGuid = "2n1$example";
  snapshot.selectedBimType = "IfcDoor";
  snapshot.selectedBimSourceId = "#42";
  snapshot.bimModelPath = "models/example.ifc";
  snapshot.bimFilter.typeFilterEnabled = true;
  snapshot.bimFilter.type = "IfcDoor";
  snapshot.bimFilter.storeyFilterEnabled = true;
  snapshot.bimFilter.storey = "Level 01";
  snapshot.bimFilter.statusFilterEnabled = true;
  snapshot.bimFilter.status = "Existing";
  snapshot.bimFilter.isolateSelection = true;
  return snapshot;
}

}  // namespace

TEST(BcfViewpoint, RoundTripsContainerSnapshotFields) {
  const auto snapshot = sampleSnapshot();
  const std::string xml =
      container::ui::bcf::ExportVisualizationInfo(snapshot);

  EXPECT_NE(xml.find("<VisualizationInfo>"), std::string::npos);
  EXPECT_NE(xml.find("<PerspectiveCamera>"), std::string::npos);
  EXPECT_NE(xml.find("IfcGuid=\"2n1$example\""), std::string::npos);

  const auto imported = container::ui::bcf::ImportVisualizationInfo(xml);
  ASSERT_TRUE(imported.has_value());
  EXPECT_EQ(imported->label, snapshot.label);
  EXPECT_FLOAT_EQ(imported->camera.position.x, snapshot.camera.position.x);
  EXPECT_FLOAT_EQ(imported->camera.position.y, snapshot.camera.position.y);
  EXPECT_FLOAT_EQ(imported->camera.position.z, snapshot.camera.position.z);
  EXPECT_FLOAT_EQ(imported->camera.rotationDegrees.x,
                  snapshot.camera.rotationDegrees.x);
  EXPECT_FLOAT_EQ(imported->camera.rotationDegrees.y,
                  snapshot.camera.rotationDegrees.y);
  EXPECT_EQ(imported->selectedMeshNode, snapshot.selectedMeshNode);
  EXPECT_EQ(imported->selectedBimObjectIndex,
            snapshot.selectedBimObjectIndex);
  EXPECT_EQ(imported->selectedBimGuid, snapshot.selectedBimGuid);
  EXPECT_EQ(imported->selectedBimType, snapshot.selectedBimType);
  EXPECT_EQ(imported->selectedBimSourceId, snapshot.selectedBimSourceId);
  EXPECT_EQ(imported->bimModelPath, snapshot.bimModelPath);
  EXPECT_TRUE(imported->bimFilter.typeFilterEnabled);
  EXPECT_EQ(imported->bimFilter.type, "IfcDoor");
  EXPECT_TRUE(imported->bimFilter.storeyFilterEnabled);
  EXPECT_EQ(imported->bimFilter.storey, "Level 01");
  EXPECT_TRUE(imported->bimFilter.statusFilterEnabled);
  EXPECT_EQ(imported->bimFilter.status, "Existing");
  EXPECT_TRUE(imported->bimFilter.isolateSelection);
}

TEST(BcfViewpoint, SavesAndLoadsVisualizationInfoFile) {
  const auto snapshot = sampleSnapshot();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "container_bcf_viewpoint.bcfv";

  ASSERT_TRUE(container::ui::bcf::SaveVisualizationInfo(snapshot, path));
  const auto imported = container::ui::bcf::LoadVisualizationInfo(path);
  ASSERT_TRUE(imported.has_value());
  EXPECT_EQ(imported->label, snapshot.label);
  EXPECT_EQ(imported->selectedBimGuid, snapshot.selectedBimGuid);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(BcfViewpoint, ImportsStandardPerspectiveCameraWithoutContainerSnapshot) {
  constexpr const char* kStandardBcf = R"xml(
<VisualizationInfo>
  <PerspectiveCamera>
    <CameraViewPoint>
      <X>10</X>
      <Y>2</Y>
      <Z>-5</Z>
    </CameraViewPoint>
    <CameraDirection>
      <X>0</X>
      <Y>0</Y>
      <Z>-1</Z>
    </CameraDirection>
    <CameraUpVector>
      <X>0</X>
      <Y>1</Y>
      <Z>0</Z>
    </CameraUpVector>
    <FieldOfView>60</FieldOfView>
  </PerspectiveCamera>
  <Components>
    <Selection>
      <Component IfcGuid="3abc" AuthoringToolId="#123"/>
    </Selection>
  </Components>
</VisualizationInfo>
)xml";

  const auto imported =
      container::ui::bcf::ImportVisualizationInfo(kStandardBcf);
  ASSERT_TRUE(imported.has_value());
  EXPECT_FLOAT_EQ(imported->camera.position.x, 10.0f);
  EXPECT_FLOAT_EQ(imported->camera.position.y, 2.0f);
  EXPECT_FLOAT_EQ(imported->camera.position.z, -5.0f);
  EXPECT_NEAR(imported->camera.rotationDegrees.x, 0.0f, 1.0e-5f);
  EXPECT_NEAR(imported->camera.rotationDegrees.y, 90.0f, 1.0e-5f);
  EXPECT_EQ(imported->selectedBimGuid, "3abc");
  EXPECT_EQ(imported->selectedBimSourceId, "#123");
}

TEST(BcfViewpoint, RoundTripsTopicCommentsAndViewpointPins) {
  container::ui::bcf::BcfMarkup markup{};
  markup.topic.guid = "topic-1";
  markup.topic.title = "Door clearance";
  markup.topic.status = "Open";
  markup.topic.priority = "High";
  markup.topic.labels = {"coordination", "door"};
  markup.topic.tags = {"egress", "clearance"};

  container::ui::bcf::BcfViewpointMarkup viewpoint{};
  viewpoint.guid = "viewpoint-1";
  viewpoint.viewpointFile = "viewpoint.bcfv";
  viewpoint.snapshotFile = "snapshot.png";
  viewpoint.pins.push_back({
      .label = "Door leaf",
      .location = {1.0f, 2.0f, 3.0f},
      .ifcGuid = "2n1$example",
      .sourceId = "#42",
  });
  markup.viewpoints.push_back(viewpoint);

  container::ui::bcf::BcfViewpointMarkup secondViewpoint{};
  secondViewpoint.guid = "viewpoint-2";
  secondViewpoint.viewpointFile = "viewpoint-2.bcfv";
  secondViewpoint.pins.push_back({
      .label = "Frame",
      .location = {2.0f, 3.0f, 4.0f},
      .ifcGuid = "3n1$frame",
      .sourceId = "#43",
  });
  markup.viewpoints.push_back(secondViewpoint);

  markup.comments.push_back({
      .guid = "comment-1",
      .date = "2026-05-03T12:00:00Z",
      .author = "Reviewer",
      .text = "Please verify the clear opening.",
      .viewpointGuid = "viewpoint-1",
  });
  markup.comments.push_back({
      .guid = "comment-2",
      .author = "Architect",
      .text = "Frame detail is the second viewpoint.",
      .viewpointGuid = "viewpoint-2",
  });

  const std::string xml = container::ui::bcf::ExportMarkup(markup);
  EXPECT_NE(xml.find("<Markup>"), std::string::npos);
  EXPECT_NE(xml.find("TopicStatus=\"Open\""), std::string::npos);
  EXPECT_NE(xml.find("IfcGuid=\"2n1$example\""), std::string::npos);
  EXPECT_NE(xml.find("<Label>coordination</Label>"), std::string::npos);
  EXPECT_NE(xml.find("<Tag>egress</Tag>"), std::string::npos);
  const std::string expectedPinGuid =
      container::ui::bcf::StablePinGuid("2n1$example", "#42", "topic-1");
  EXPECT_NE(xml.find("Guid=\"" + expectedPinGuid + "\""), std::string::npos);

  const auto imported = container::ui::bcf::ImportMarkup(xml);
  ASSERT_TRUE(imported.has_value());
  EXPECT_EQ(imported->topic.guid, "topic-1");
  EXPECT_EQ(imported->topic.title, "Door clearance");
  EXPECT_EQ(imported->topic.status, "Open");
  EXPECT_EQ(imported->topic.priority, "High");
  ASSERT_EQ(imported->topic.labels.size(), 2u);
  EXPECT_EQ(imported->topic.labels.front(), "coordination");
  ASSERT_EQ(imported->topic.tags.size(), 2u);
  EXPECT_EQ(imported->topic.tags.front(), "egress");

  ASSERT_EQ(imported->comments.size(), 2u);
  EXPECT_EQ(imported->comments.front().guid, "comment-1");
  EXPECT_EQ(imported->comments.front().author, "Reviewer");
  EXPECT_EQ(imported->comments.front().text,
            "Please verify the clear opening.");
  EXPECT_EQ(imported->comments.front().viewpointGuid, "viewpoint-1");
  EXPECT_EQ(imported->comments.back().viewpointGuid, "viewpoint-2");

  ASSERT_EQ(imported->viewpoints.size(), 2u);
  EXPECT_EQ(imported->viewpoints.front().guid, "viewpoint-1");
  ASSERT_EQ(imported->viewpoints.front().pins.size(), 1u);
  const container::ui::bcf::BcfPin& pin =
      imported->viewpoints.front().pins.front();
  EXPECT_EQ(pin.guid, expectedPinGuid);
  EXPECT_EQ(pin.label, "Door leaf");
  EXPECT_FLOAT_EQ(pin.location.x, 1.0f);
  EXPECT_FLOAT_EQ(pin.location.y, 2.0f);
  EXPECT_FLOAT_EQ(pin.location.z, 3.0f);
  EXPECT_EQ(pin.ifcGuid, "2n1$example");
  EXPECT_EQ(pin.sourceId, "#42");
  ASSERT_EQ(imported->viewpoints.back().pins.size(), 1u);
  EXPECT_EQ(imported->viewpoints.back().pins.front().guid,
            container::ui::bcf::StablePinGuid("3n1$frame", "#43", "topic-1"));
}

TEST(BcfViewpoint, VisualizationInfoPinsDoNotBreakSnapshotImport) {
  const auto snapshot = sampleSnapshot();
  container::ui::bcf::BcfViewpointMarkup markup{};
  markup.guid = "viewpoint-1";
  markup.pins.push_back({
      .guid = "pin-1",
      .label = "Selected door",
      .location = {4.0f, 5.0f, 6.0f},
      .ifcGuid = snapshot.selectedBimGuid,
      .sourceId = snapshot.selectedBimSourceId,
  });

  const std::string xml =
      container::ui::bcf::ExportVisualizationInfo(snapshot, markup);

  const auto importedSnapshot =
      container::ui::bcf::ImportVisualizationInfo(xml);
  ASSERT_TRUE(importedSnapshot.has_value());
  EXPECT_EQ(importedSnapshot->label, snapshot.label);
  EXPECT_EQ(importedSnapshot->selectedBimGuid, snapshot.selectedBimGuid);
  EXPECT_EQ(importedSnapshot->selectedBimSourceId,
            snapshot.selectedBimSourceId);

  const auto importedMarkup =
      container::ui::bcf::ImportVisualizationInfoMarkup(xml);
  ASSERT_TRUE(importedMarkup.has_value());
  EXPECT_EQ(importedMarkup->guid, "viewpoint-1");
  ASSERT_EQ(importedMarkup->pins.size(), 1u);
  EXPECT_EQ(importedMarkup->pins.front().ifcGuid, snapshot.selectedBimGuid);
  EXPECT_EQ(importedMarkup->pins.front().sourceId,
            snapshot.selectedBimSourceId);
  EXPECT_FLOAT_EQ(importedMarkup->pins.front().location.x, 4.0f);
}

TEST(BcfViewpoint, SavesAndLoadsUnpackedTopicFolder) {
  const auto snapshot = sampleSnapshot();
  auto secondSnapshot = snapshot;
  secondSnapshot.label = "Frame review";
  secondSnapshot.selectedBimGuid = "3n1$frame";
  secondSnapshot.selectedBimSourceId = "#43";
  secondSnapshot.camera.position = {7.0f, 8.0f, 9.0f};

  container::ui::bcf::BcfTopicFolder folder{};
  folder.markup.topic.guid = "topic-folder";
  folder.markup.topic.title = "Door and frame";
  folder.markup.topic.status = "Open";
  folder.markup.topic.labels = {"coordination"};

  container::ui::bcf::BcfTopicViewpoint first{};
  first.markup.guid = "viewpoint-a";
  first.markup.viewpointFile = "viewpoint-a.bcfv";
  first.markup.pins.push_back({
      .label = "Door",
      .location = {1.0f, 2.0f, 3.0f},
      .ifcGuid = snapshot.selectedBimGuid,
      .sourceId = snapshot.selectedBimSourceId,
  });
  first.snapshot = snapshot;
  folder.viewpoints.push_back(first);

  container::ui::bcf::BcfTopicViewpoint second{};
  second.markup.guid = "viewpoint-b";
  second.markup.viewpointFile = "viewpoint-b.bcfv";
  second.markup.pins.push_back({
      .label = "Frame",
      .location = {7.0f, 8.0f, 9.0f},
      .ifcGuid = secondSnapshot.selectedBimGuid,
      .sourceId = secondSnapshot.selectedBimSourceId,
  });
  second.snapshot = secondSnapshot;
  folder.viewpoints.push_back(second);

  folder.markup.comments.push_back({
      .guid = "comment-a",
      .author = "Reviewer",
      .text = "Door issue",
      .viewpointGuid = "viewpoint-a",
  });
  folder.markup.comments.push_back({
      .guid = "comment-b",
      .author = "Reviewer",
      .text = "Frame issue",
      .viewpointGuid = "viewpoint-b",
  });

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "container_bcf_topic_folder";
  std::error_code ec;
  std::filesystem::remove_all(path, ec);

  ASSERT_TRUE(container::ui::bcf::SaveTopicFolder(folder, path));
  EXPECT_TRUE(std::filesystem::exists(path / "markup.bcf"));
  EXPECT_TRUE(std::filesystem::exists(path / "viewpoint-a.bcfv"));
  EXPECT_TRUE(std::filesystem::exists(path / "viewpoint-b.bcfv"));

  const auto loaded = container::ui::bcf::LoadTopicFolder(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->markup.topic.title, "Door and frame");
  ASSERT_EQ(loaded->markup.topic.labels.size(), 1u);
  EXPECT_EQ(loaded->markup.topic.labels.front(), "coordination");
  ASSERT_EQ(loaded->markup.comments.size(), 2u);
  EXPECT_EQ(loaded->markup.comments.front().viewpointGuid, "viewpoint-a");
  EXPECT_EQ(loaded->markup.comments.back().viewpointGuid, "viewpoint-b");

  ASSERT_EQ(loaded->viewpoints.size(), 2u);
  ASSERT_TRUE(loaded->viewpoints.front().snapshot.has_value());
  EXPECT_EQ(loaded->viewpoints.front().snapshot->selectedBimGuid,
            snapshot.selectedBimGuid);
  ASSERT_TRUE(loaded->viewpoints.back().snapshot.has_value());
  EXPECT_EQ(loaded->viewpoints.back().snapshot->selectedBimGuid,
            secondSnapshot.selectedBimGuid);
  ASSERT_EQ(loaded->viewpoints.front().markup.pins.size(), 1u);
  EXPECT_EQ(loaded->viewpoints.front().markup.pins.front().guid,
            container::ui::bcf::StablePinGuid(
                snapshot.selectedBimGuid, snapshot.selectedBimSourceId,
                "topic-folder"));

  std::filesystem::remove_all(path, ec);
}

TEST(BcfViewpoint, SavesAndLoadsBcfArchive) {
  const auto snapshot = sampleSnapshot();
  auto secondSnapshot = snapshot;
  secondSnapshot.label = "Window review";
  secondSnapshot.selectedBimGuid = "4n1$window";
  secondSnapshot.selectedBimSourceId = "#44";
  secondSnapshot.camera.position = {10.0f, 11.0f, 12.0f};

  container::ui::bcf::BcfTopicFolder archive{};
  archive.markup.topic.guid = "archive-topic";
  archive.markup.topic.title = "Archive review";
  archive.markup.topic.status = "Open";
  archive.markup.topic.priority = "Normal";
  archive.markup.topic.labels = {"coordination", "envelope"};
  archive.markup.topic.tags = {"door", "window"};

  container::ui::bcf::BcfTopicViewpoint first{};
  first.markup.guid = "archive-viewpoint-a";
  first.markup.viewpointFile = "viewpoint-a.bcfv";
  first.markup.pins.push_back({
      .label = "Door pin",
      .location = {1.0f, 2.0f, 3.0f},
      .ifcGuid = snapshot.selectedBimGuid,
      .sourceId = snapshot.selectedBimSourceId,
  });
  first.snapshot = snapshot;
  archive.viewpoints.push_back(first);

  container::ui::bcf::BcfTopicViewpoint second{};
  second.markup.guid = "archive-viewpoint-b";
  second.markup.viewpointFile = "viewpoint-b.bcfv";
  second.markup.pins.push_back({
      .label = "Window pin",
      .location = {4.0f, 5.0f, 6.0f},
      .ifcGuid = secondSnapshot.selectedBimGuid,
      .sourceId = secondSnapshot.selectedBimSourceId,
  });
  second.snapshot = secondSnapshot;
  archive.viewpoints.push_back(second);

  archive.markup.comments.push_back({
      .guid = "archive-comment-a",
      .author = "Reviewer",
      .text = "Door archive comment",
      .viewpointGuid = "archive-viewpoint-a",
  });
  archive.markup.comments.push_back({
      .guid = "archive-comment-b",
      .author = "Reviewer",
      .text = "Window archive comment",
      .viewpointGuid = "archive-viewpoint-b",
  });

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "container_bcf_topic.bcf";
  std::error_code ec;
  std::filesystem::remove(path, ec);

  ASSERT_TRUE(container::ui::bcf::SaveBcfArchive(archive, path));
  EXPECT_TRUE(std::filesystem::exists(path));

  const auto loaded = container::ui::bcf::LoadBcfArchive(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->markup.topic.guid, "archive-topic");
  EXPECT_EQ(loaded->markup.topic.title, "Archive review");
  EXPECT_EQ(loaded->markup.topic.status, "Open");
  EXPECT_EQ(loaded->markup.topic.priority, "Normal");
  ASSERT_EQ(loaded->markup.topic.labels.size(), 2u);
  EXPECT_EQ(loaded->markup.topic.labels.front(), "coordination");
  ASSERT_EQ(loaded->markup.topic.tags.size(), 2u);
  EXPECT_EQ(loaded->markup.topic.tags.back(), "window");

  ASSERT_EQ(loaded->markup.comments.size(), 2u);
  EXPECT_EQ(loaded->markup.comments.front().viewpointGuid,
            "archive-viewpoint-a");
  EXPECT_EQ(loaded->markup.comments.back().viewpointGuid,
            "archive-viewpoint-b");

  ASSERT_EQ(loaded->viewpoints.size(), 2u);
  ASSERT_TRUE(loaded->viewpoints.front().snapshot.has_value());
  EXPECT_EQ(loaded->viewpoints.front().snapshot->selectedBimGuid,
            snapshot.selectedBimGuid);
  ASSERT_TRUE(loaded->viewpoints.back().snapshot.has_value());
  EXPECT_EQ(loaded->viewpoints.back().snapshot->selectedBimGuid,
            secondSnapshot.selectedBimGuid);

  ASSERT_EQ(loaded->viewpoints.front().markup.pins.size(), 1u);
  EXPECT_EQ(loaded->viewpoints.front().markup.pins.front().guid,
            container::ui::bcf::StablePinGuid(
                snapshot.selectedBimGuid, snapshot.selectedBimSourceId,
                "archive-topic"));
  EXPECT_EQ(loaded->viewpoints.front().markup.pins.front().ifcGuid,
            snapshot.selectedBimGuid);
  ASSERT_EQ(loaded->viewpoints.back().markup.pins.size(), 1u);
  EXPECT_EQ(loaded->viewpoints.back().markup.pins.front().sourceId,
            secondSnapshot.selectedBimSourceId);

  std::filesystem::remove(path, ec);
}
