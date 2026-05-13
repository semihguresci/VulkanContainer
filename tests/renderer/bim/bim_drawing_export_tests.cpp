#include "Container/renderer/bim/BimDrawingExport.h"

#include <gtest/gtest.h>

#include <limits>

using container::renderer::BimDrawingExportLine;
using container::renderer::BimDrawingExportRequest;
using container::renderer::ExportBimDrawingSvg;

TEST(BimDrawingExportTests, WritesSvgWithScaleViewAndLinework) {
  BimDrawingExportRequest request{};
  request.title = "Main floor";
  request.viewName = "Front elevation";
  request.paperWidthMm = 297.0f;
  request.paperHeightMm = 210.0f;
  request.modelUnitsPerPaperMm = 50.0f;
  request.lines.push_back(BimDrawingExportLine{
      .a = {0.0f, 0.0f, 0.0f},
      .b = {4.0f, 0.0f, 0.0f},
      .color = {0.0f, 0.0f, 0.0f},
      .lineWidthMm = 0.18f,
      .layer = "walls"});

  const std::string svg = ExportBimDrawingSvg(request);

  EXPECT_NE(svg.find("<svg"), std::string::npos);
  EXPECT_NE(svg.find("Main floor"), std::string::npos);
  EXPECT_NE(svg.find("Front elevation"), std::string::npos);
  EXPECT_NE(svg.find("data-layer=\"walls\""), std::string::npos);
  EXPECT_NE(svg.find("<line"), std::string::npos);
}

TEST(BimDrawingExportTests, EscapesXmlMetadataAndLayerNames) {
  BimDrawingExportRequest request{};
  request.title = "A&B <Main> \"Plan\"";
  request.viewName = "Front's & Section";
  request.lines.push_back(BimDrawingExportLine{
      .a = {0.0f, 0.0f, 0.0f},
      .b = {1.0f, 0.0f, 1.0f},
      .layer = "<walls&doors>"});

  const std::string svg = ExportBimDrawingSvg(request);

  EXPECT_NE(svg.find("A&amp;B &lt;Main&gt; &quot;Plan&quot;"),
            std::string::npos);
  EXPECT_NE(svg.find("Front&apos;s &amp; Section"), std::string::npos);
  EXPECT_NE(svg.find("data-layer=\"&lt;walls&amp;doors&gt;\""),
            std::string::npos);
}

TEST(BimDrawingExportTests, CentersXzProjectionOnPaper) {
  BimDrawingExportRequest request{};
  request.paperWidthMm = 100.0f;
  request.paperHeightMm = 80.0f;
  request.modelUnitsPerPaperMm = 2.0f;
  request.lines.push_back(BimDrawingExportLine{
      .a = {-4.0f, 0.0f, -6.0f},
      .b = {8.0f, 0.0f, 10.0f}});

  const std::string svg = ExportBimDrawingSvg(request);

  EXPECT_NE(svg.find("x1=\"48.000\""), std::string::npos);
  EXPECT_NE(svg.find("y1=\"43.000\""), std::string::npos);
  EXPECT_NE(svg.find("x2=\"54.000\""), std::string::npos);
  EXPECT_NE(svg.find("y2=\"35.000\""), std::string::npos);
}

TEST(BimDrawingExportTests, ClampsLineWidthToMinimum) {
  BimDrawingExportRequest request{};
  request.lines.push_back(BimDrawingExportLine{
      .a = {0.0f, 0.0f, 0.0f},
      .b = {1.0f, 0.0f, 1.0f},
      .lineWidthMm = 0.01f});

  const std::string svg = ExportBimDrawingSvg(request);

  EXPECT_NE(svg.find("stroke-width=\"0.050mm\""), std::string::npos);
}

TEST(BimDrawingExportTests, ClampsRgbColorComponents) {
  BimDrawingExportRequest request{};
  request.lines.push_back(BimDrawingExportLine{
      .a = {0.0f, 0.0f, 0.0f},
      .b = {1.0f, 0.0f, 1.0f},
      .color = {-0.25f, 0.5f, 1.5f}});

  const std::string svg = ExportBimDrawingSvg(request);

  EXPECT_NE(svg.find("stroke=\"rgb(0,128,255)\""), std::string::npos);
}

TEST(BimDrawingExportTests, RejectsEmptyPaperSize) {
  BimDrawingExportRequest request{};
  request.paperWidthMm = 0.0f;
  request.paperHeightMm = 210.0f;

  EXPECT_TRUE(ExportBimDrawingSvg(request).empty());
}

TEST(BimDrawingExportTests, RejectsNonPositivePaperHeight) {
  BimDrawingExportRequest request{};
  request.paperWidthMm = 297.0f;
  request.paperHeightMm = 0.0f;

  EXPECT_TRUE(ExportBimDrawingSvg(request).empty());
}

TEST(BimDrawingExportTests, RejectsNonPositiveScale) {
  BimDrawingExportRequest request{};
  request.paperWidthMm = 297.0f;
  request.paperHeightMm = 210.0f;
  request.modelUnitsPerPaperMm = 0.0f;

  EXPECT_TRUE(ExportBimDrawingSvg(request).empty());
}

TEST(BimDrawingExportTests, RejectsNonFinitePaperSizeAndScale) {
  const float inf = std::numeric_limits<float>::infinity();
  const float nan = std::numeric_limits<float>::quiet_NaN();

  BimDrawingExportRequest request{};
  request.paperWidthMm = inf;
  EXPECT_TRUE(ExportBimDrawingSvg(request).empty());

  request = {};
  request.paperHeightMm = nan;
  EXPECT_TRUE(ExportBimDrawingSvg(request).empty());

  request = {};
  request.modelUnitsPerPaperMm = inf;
  EXPECT_TRUE(ExportBimDrawingSvg(request).empty());
}

TEST(BimDrawingExportTests, SanitizesNonFiniteLineValues) {
  const float inf = std::numeric_limits<float>::infinity();
  const float nan = std::numeric_limits<float>::quiet_NaN();

  BimDrawingExportRequest request{};
  request.lines.push_back(BimDrawingExportLine{
      .a = {nan, 0.0f, inf},
      .b = {-inf, 0.0f, nan},
      .color = {nan, inf, -inf},
      .lineWidthMm = inf});

  const std::string svg = ExportBimDrawingSvg(request);

  EXPECT_NE(svg.find("<line"), std::string::npos);
  EXPECT_EQ(svg.find("nan"), std::string::npos);
  EXPECT_EQ(svg.find("inf"), std::string::npos);
  EXPECT_NE(svg.find("x1=\"148.500\""), std::string::npos);
  EXPECT_NE(svg.find("y1=\"105.000\""), std::string::npos);
  EXPECT_NE(svg.find("x2=\"148.500\""), std::string::npos);
  EXPECT_NE(svg.find("y2=\"105.000\""), std::string::npos);
  EXPECT_NE(svg.find("stroke=\"rgb(0,0,0)\""), std::string::npos);
  EXPECT_NE(svg.find("stroke-width=\"0.050mm\""), std::string::npos);
}

TEST(BimDrawingExportTests, SanitizesProjectionOverflowFromTinyScale) {
  BimDrawingExportRequest request{};
  request.modelUnitsPerPaperMm = std::numeric_limits<float>::min();
  request.lines.push_back(BimDrawingExportLine{
      .a = {std::numeric_limits<float>::max(), 0.0f,
            std::numeric_limits<float>::max()},
      .b = {-std::numeric_limits<float>::max(), 0.0f,
            -std::numeric_limits<float>::max()}});

  const std::string svg = ExportBimDrawingSvg(request);

  EXPECT_NE(svg.find("<line"), std::string::npos);
  EXPECT_EQ(svg.find("inf"), std::string::npos);
  EXPECT_EQ(svg.find("nan"), std::string::npos);
  EXPECT_NE(svg.find("x1=\"148.500\""), std::string::npos);
  EXPECT_NE(svg.find("y1=\"105.000\""), std::string::npos);
  EXPECT_NE(svg.find("x2=\"148.500\""), std::string::npos);
  EXPECT_NE(svg.find("y2=\"105.000\""), std::string::npos);
}
