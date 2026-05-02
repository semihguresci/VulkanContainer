#include "Container/geometry/IfcTessellatedLoader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <glm/geometric.hpp>

#ifndef CONTAINER_BINARY_DIR
#define CONTAINER_BINARY_DIR "."
#endif

namespace {

TEST(IfcTessellatedLoader, ParsesPlacementUnitsAndIndexedColors) {
  constexpr const char *kIfc = R"ifc(
ISO-10303-21;
DATA;
#1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);
#10=IFCCARTESIANPOINT((1000.,2000.,0.));
#11=IFCDIRECTION((0.,0.,1.));
#12=IFCDIRECTION((1.,0.,0.));
#13=IFCAXIS2PLACEMENT3D(#10,#11,#12);
#14=IFCLOCALPLACEMENT($,#13);
#20=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(1000.,0.,0.),(0.,1000.,0.),(0.,0.,1000.)));
#21=IFCTRIANGULATEDFACESET(#20,$,.T.,((1,2,3),(1,3,4)),$);
#22=IFCCOLOURRGBLIST(((1.,0.,0.),(0.,1.,0.)));
#23=IFCINDEXEDCOLOURMAP(#21,$,#22,(1,2));
#30=IFCSHAPEREPRESENTATION($,'Body','Tessellation',(#21));
#31=IFCPRODUCTDEFINITIONSHAPE($,$,(#30));
#40=IFCBUILDINGELEMENTPROXY('proxy-guid',$,'Proxy',$,$,#14,#31,$,$);
ENDSEC;
END-ISO-10303-21;
)ifc";

  const auto model = container::geometry::ifc::LoadFromStep(kIfc, 2.0f);

  ASSERT_EQ(model.meshRanges.size(), 2u);
  ASSERT_EQ(model.elements.size(), 2u);
  EXPECT_EQ(model.vertices.size(), 6u);
  EXPECT_EQ(model.indices.size(), 6u);

  EXPECT_NEAR(model.elements[0].transform[0].x, 0.002f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[1].z, -0.002f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[2].y, 0.002f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[3].x, 2.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[3].y, 0.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[3].z, -4.0f, 1.0e-6f);
  EXPECT_EQ(model.elements[0].guid, "proxy-guid");
  EXPECT_EQ(model.elements[0].type, "IFCBUILDINGELEMENTPROXY");

  const bool hasRed =
      (model.elements[0].color.r > 0.9f && model.elements[0].color.g < 0.1f) ||
      (model.elements[1].color.r > 0.9f && model.elements[1].color.g < 0.1f);
  const bool hasGreen =
      (model.elements[0].color.g > 0.9f && model.elements[0].color.r < 0.1f) ||
      (model.elements[1].color.g > 0.9f && model.elements[1].color.r < 0.1f);
  EXPECT_TRUE(hasRed);
  EXPECT_TRUE(hasGreen);

  EXPECT_NEAR(glm::length(model.vertices[0].normal), 1.0f, 1.0e-5f);
}

TEST(IfcTessellatedLoader, ParsesMappedItemTransformAndStyleColor) {
  constexpr const char *kIfc = R"ifc(
ISO-10303-21;
DATA;
#1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);
#10=IFCCARTESIANPOINT((0.,0.,0.));
#11=IFCDIRECTION((0.,0.,1.));
#12=IFCDIRECTION((1.,0.,0.));
#13=IFCAXIS2PLACEMENT3D(#10,#11,#12);
#14=IFCLOCALPLACEMENT($,#13);
#20=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(1000.,0.,0.),(0.,1000.,0.)));
#21=IFCTRIANGULATEDFACESET(#20,$,.T.,((1,2,3)),$);
#22=IFCCOLOURRGB($,0.25,0.5,0.75);
#23=IFCSURFACESTYLERENDERING(#22,0.,$,$,$,$,$,$,.NOTDEFINED.);
#24=IFCSURFACESTYLE('paint',.BOTH.,(#23));
#25=IFCSTYLEDITEM(#21,(#24),$);
#30=IFCSHAPEREPRESENTATION($,'Body','Tessellation',(#21));
#31=IFCREPRESENTATIONMAP(#13,#30);
#40=IFCDIRECTION((1.,0.,0.));
#41=IFCDIRECTION((0.,1.,0.));
#42=IFCCARTESIANPOINT((1000.,0.,0.));
#43=IFCDIRECTION((0.,0.,1.));
#44=IFCCARTESIANTRANSFORMATIONOPERATOR3D(#40,#41,#42,1.,#43);
#50=IFCMAPPEDITEM(#31,#44);
#51=IFCSHAPEREPRESENTATION($,'Body','MappedRepresentation',(#50));
#52=IFCPRODUCTDEFINITIONSHAPE($,$,(#51));
#60=IFCBUILDINGELEMENTPROXY('mapped-guid',$,'Proxy',$,$,#14,#52,$,$);
ENDSEC;
END-ISO-10303-21;
)ifc";

  const auto model = container::geometry::ifc::LoadFromStep(kIfc);

  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_NEAR(model.elements[0].transform[3].x, 1.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.r, 0.25f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.5f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.75f, 1.0e-6f);
}

TEST(IfcTessellatedLoader, AppliesRectangularOpeningVoidToSweptSolidWall) {
  constexpr const char *kIfc = R"ifc(
ISO-10303-21;
DATA;
#1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);
#10=IFCCARTESIANPOINT((0.,0.,0.));
#11=IFCDIRECTION((0.,0.,1.));
#12=IFCDIRECTION((1.,0.,0.));
#13=IFCAXIS2PLACEMENT3D(#10,#11,#12);
#14=IFCLOCALPLACEMENT($,#13);
#20=IFCCARTESIANPOINT((0.,0.,0.));
#21=IFCCARTESIANPOINT((1000.,0.,0.));
#22=IFCCARTESIANPOINT((1000.,100.,0.));
#23=IFCCARTESIANPOINT((0.,100.,0.));
#24=IFCPOLYLINE((#20,#21,#22,#23,#20));
#25=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#24);
#26=IFCEXTRUDEDAREASOLID(#25,#13,#11,1000.);
#27=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#26));
#28=IFCPRODUCTDEFINITIONSHAPE($,$,(#27));
#29=IFCWALL('wall-guid',$,$,$,$,#14,#28,$,$);
#30=IFCCARTESIANPOINT((250.,0.,250.));
#31=IFCAXIS2PLACEMENT3D(#30,#11,#12);
#32=IFCLOCALPLACEMENT(#14,#31);
#40=IFCCARTESIANPOINT((0.,0.,0.));
#41=IFCCARTESIANPOINT((500.,0.,0.));
#42=IFCCARTESIANPOINT((500.,100.,0.));
#43=IFCCARTESIANPOINT((0.,100.,0.));
#44=IFCPOLYLINE((#40,#41,#42,#43,#40));
#45=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#44);
#46=IFCEXTRUDEDAREASOLID(#45,#13,#11,500.);
#47=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#46));
#48=IFCPRODUCTDEFINITIONSHAPE($,$,(#47));
#49=IFCOPENINGELEMENT('opening-guid',$,$,$,$,#32,#48,$);
#50=IFCRELVOIDSELEMENT('void-guid',$,$,$,#29,#49);
ENDSEC;
END-ISO-10303-21;
)ifc";

  const auto model = container::geometry::ifc::LoadFromStep(kIfc);

  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.elements[0].type, "IFCWALL");
  EXPECT_EQ(model.elements[0].guid, "wall-guid");
  const auto wallRangeIt =
      std::ranges::find_if(model.meshRanges, [&](const auto &range) {
        return range.meshId == model.elements[0].meshId;
      });
  ASSERT_NE(wallRangeIt, model.meshRanges.end());
  EXPECT_GT(wallRangeIt->indexCount, 36u);
}

} // namespace
