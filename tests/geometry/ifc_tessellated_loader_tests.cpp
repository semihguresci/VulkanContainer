#include "Container/geometry/IfcTessellatedLoader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

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

  EXPECT_TRUE(model.unitMetadata.hasSourceUnits);
  EXPECT_EQ(model.unitMetadata.sourceUnits, "millimetre");
  EXPECT_TRUE(model.unitMetadata.hasMetersPerUnit);
  EXPECT_NEAR(model.unitMetadata.metersPerUnit, 0.001f, 1.0e-9f);
  EXPECT_TRUE(model.unitMetadata.hasImportScale);
  EXPECT_NEAR(model.unitMetadata.importScale, 2.0f, 1.0e-6f);
  EXPECT_TRUE(model.unitMetadata.hasEffectiveImportScale);
  EXPECT_NEAR(model.unitMetadata.effectiveImportScale, 0.002f, 1.0e-9f);

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

TEST(IfcTessellatedLoader, PreservesProductStoreyAndMaterialMetadata) {
  constexpr const char *kIfc = R"ifc(
ISO-10303-21;
DATA;
#1=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);
#10=IFCCARTESIANPOINT((0.,0.,0.));
#11=IFCDIRECTION((0.,0.,1.));
#12=IFCDIRECTION((1.,0.,0.));
#13=IFCAXIS2PLACEMENT3D(#10,#11,#12);
#14=IFCLOCALPLACEMENT($,#13);
#20=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(1.,0.,0.),(0.,1.,0.)));
#21=IFCTRIANGULATEDFACESET(#20,$,.T.,((1,2,3)),$);
#30=IFCSHAPEREPRESENTATION($,'Body','Tessellation',(#21));
#31=IFCPRODUCTDEFINITIONSHAPE($,$,(#30));
#40=IFCBUILDINGELEMENTPROXY('proxy-guid',$,'Proxy Name',$,'Proxy Object Type',#14,#31,$,$);
#50=IFCBUILDINGSTOREY('storey-guid',$,'Level 01',$,$,$,$,$,$);
#51=IFCRELCONTAINEDINSPATIALSTRUCTURE('containment-guid',$,$,$,(#40),#50);
#60=IFCMATERIAL('Concrete',$,'Structural');
#61=IFCRELASSOCIATESMATERIAL('material-guid',$,$,$,(#40),#60);
#70=IFCPROPERTYSINGLEVALUE('Discipline',$,IFCLABEL('Architecture'),$);
#71=IFCPROPERTYSINGLEVALUE('Phase',$,IFCLABEL('New construction'),$);
#72=IFCPROPERTYSINGLEVALUE('FireRating',$,IFCLABEL('2h'),$);
#73=IFCPROPERTYSINGLEVALUE('LoadBearing',$,IFCBOOLEAN(.T.),$);
#74=IFCPROPERTYSINGLEVALUE('Status',$,IFCLABEL('Existing'),$);
#77=IFCPROPERTYSINGLEVALUE('AcousticRating',$,IFCLABEL('Rw40'),$);
#78=IFCPROPERTYENUMERATEDVALUE('Combustible',$,(IFCLABEL('No')),$);
#75=IFCPROPERTYSET('pset-guid',$,'Pset_WallCommon',$,(#70,#71,#72,#73,#74,#77,#78));
#76=IFCRELDEFINESBYPROPERTIES('props-guid',$,$,$,(#40),#75);
#80=IFCQUANTITYLENGTH('Height',$,$,3.5,$);
#81=IFCQUANTITYAREA('GrossSideArea',$,$,12.25,$);
#82=IFCELEMENTQUANTITY('quantity-guid',$,'BaseQuantities',$,$,(#80,#81));
#83=IFCRELDEFINESBYPROPERTIES('quantity-rel-guid',$,$,$,(#40),#82);
#90=IFCCLASSIFICATION($,$,$,'Uniclass2015',$,$,$);
#91=IFCCLASSIFICATIONREFERENCE($,'Ss_25_10_30','Wall classification',#90);
#92=IFCRELASSOCIATESCLASSIFICATION('class-rel-guid',$,$,$,(#40),#91);
#95=IFCSYSTEM('system-guid',$,'HVAC System',$,$);
#96=IFCRELASSIGNSTOGROUP('system-rel-guid',$,$,$,(#40),$,#95);
#97=IFCZONE('zone-guid',$,'Thermal Zone',$,$);
#98=IFCRELASSIGNSTOGROUP('zone-rel-guid',$,$,$,(#40),$,#97);
ENDSEC;
END-ISO-10303-21;
)ifc";

  const auto model = container::geometry::ifc::LoadFromStep(kIfc);

  ASSERT_EQ(model.elements.size(), 1u);
  const auto &element = model.elements[0];
  EXPECT_EQ(element.guid, "proxy-guid");
  EXPECT_EQ(element.type, "IFCBUILDINGELEMENTPROXY");
  EXPECT_EQ(element.displayName, "Proxy Name");
  EXPECT_EQ(element.objectType, "Proxy Object Type");
  EXPECT_EQ(element.storeyId, "storey-guid");
  EXPECT_EQ(element.storeyName, "Level 01");
  EXPECT_EQ(element.materialName, "Concrete");
  EXPECT_EQ(element.materialCategory, "Structural");
  EXPECT_EQ(element.discipline, "Architecture");
  EXPECT_EQ(element.phase, "New construction");
  EXPECT_EQ(element.fireRating, "2h");
  EXPECT_EQ(element.loadBearing, "true");
  EXPECT_EQ(element.status, "Existing");
  EXPECT_EQ(element.sourceId, "#40");

  const auto findProperty = [&](std::string_view set, std::string_view name,
                                std::string_view category) {
    return std::ranges::find_if(element.properties, [&](const auto &property) {
      return property.set == set && property.name == name &&
             property.category == category;
    });
  };
  const auto acoustic =
      findProperty("Pset_WallCommon", "AcousticRating", "pset");
  ASSERT_NE(acoustic, element.properties.end());
  EXPECT_EQ(acoustic->value, "Rw40");
  const auto combustible =
      findProperty("Pset_WallCommon", "Combustible", "pset");
  ASSERT_NE(combustible, element.properties.end());
  EXPECT_EQ(combustible->value, "No");
  const auto height = findProperty("BaseQuantities", "Height", "quantity");
  ASSERT_NE(height, element.properties.end());
  EXPECT_EQ(height->value, "3.500000");
  const auto grossSideArea =
      findProperty("BaseQuantities", "GrossSideArea", "quantity");
  ASSERT_NE(grossSideArea, element.properties.end());
  EXPECT_EQ(grossSideArea->value, "12.250000");
  const auto classification =
      findProperty("Uniclass2015", "Wall classification", "classification");
  ASSERT_NE(classification, element.properties.end());
  EXPECT_EQ(classification->value, "Ss_25_10_30");
  const auto system = findProperty("IFCSYSTEM", "System", "reference");
  ASSERT_NE(system, element.properties.end());
  EXPECT_EQ(system->value, "HVAC System");
  const auto zone = findProperty("IFCZONE", "Zone", "reference");
  ASSERT_NE(zone, element.properties.end());
  EXPECT_EQ(zone->value, "Thermal Zone");
  const auto storeyReference = findProperty(
      "IFCRELCONTAINEDINSPATIALSTRUCTURE", "BuildingStorey", "reference");
  ASSERT_NE(storeyReference, element.properties.end());
  EXPECT_EQ(storeyReference->value, "Level 01");
}

TEST(IfcTessellatedLoader, PreservesBrowsingRelationshipAndMaterialMetadata) {
  constexpr const char *kIfc = R"ifc(
ISO-10303-21;
DATA;
#1=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);
#10=IFCCARTESIANPOINT((0.,0.,0.));
#11=IFCDIRECTION((0.,0.,1.));
#12=IFCDIRECTION((1.,0.,0.));
#13=IFCAXIS2PLACEMENT3D(#10,#11,#12);
#14=IFCLOCALPLACEMENT($,#13);
#20=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(1.,0.,0.),(0.,1.,0.)));
#21=IFCTRIANGULATEDFACESET(#20,$,.T.,((1,2,3)),$);
#30=IFCSHAPEREPRESENTATION($,'Body','Tessellation',(#21));
#31=IFCPRODUCTDEFINITIONSHAPE($,$,(#30));
#40=IFCWALL('wall-guid',$,'Wall Instance',$,'Wall Occurrence',#14,#31,$,$);
#44=IFCBEAM('beam-guid',$,'Beam Instance',$,$,#14,#31,$,$);
#70=IFCPROPERTYSINGLEVALUE('TypeMark',$,IFCLABEL('WT-01'),$);
#71=IFCPROPERTYSET('type-pset-guid',$,'Pset_TypeCommon',$,(#70));
#72=IFCWALLTYPE('wall-type-guid',$,'Wall Type A',$,$,(#71),$,$,.STANDARD.);
#73=IFCRELDEFINESBYTYPE('type-rel-guid',$,$,$,(#40),#72);
#80=IFCMATERIAL('Gypsum Board',$,'Finish');
#81=IFCMATERIAL('Concrete',$,'Core');
#82=IFCMATERIALLAYER(#80,0.012,$,'Interior Finish',$,'Finish',1);
#83=IFCMATERIALLAYER(#81,0.200,$,'Concrete Core',$,'Core',2);
#84=IFCMATERIALLAYERSET((#82,#83),'Wall Layer Set',$);
#85=IFCMATERIALLAYERSETUSAGE(#84,.AXIS2.,.POSITIVE.,0.0,$);
#86=IFCRELASSOCIATESMATERIAL('wall-material-rel-guid',$,$,$,(#72),#85);
#90=IFCMATERIAL('Steel',$,'Metal');
#91=IFCCIRCLEPROFILEDEF(.AREA.,'Round Profile',$,0.1);
#92=IFCMATERIALPROFILE('Primary Profile',$,#90,#91,$,'Structural');
#93=IFCMATERIALPROFILESET('Beam Profile Set',$,(#92),$);
#94=IFCMATERIALPROFILESETUSAGE(#93,5,2.5);
#95=IFCRELASSOCIATESMATERIAL('beam-material-rel-guid',$,$,$,(#44),#94);
#100=IFCBUILDINGELEMENTPROXY('assembly-guid',$,'Assembly',$,$,#14,#31,$,$);
#101=IFCRELAGGREGATES('aggregate-rel-guid',$,$,$,#100,(#40,#44));
#102=IFCBUILDINGELEMENTPROXY('nested-guid',$,'Nested Part',$,$,#14,#31,$,$);
#103=IFCRELNESTS('nest-rel-guid',$,$,$,#40,(#102));
#110=IFCOPENINGELEMENT('opening-guid',$,'Window Opening',$,$,#14,#31,$);
#111=IFCRELVOIDSELEMENT('void-rel-guid',$,$,$,#40,#110);
#112=IFCWINDOW('window-guid',$,'Window',$,$,#14,#31,$,$);
#113=IFCRELFILLSELEMENT('fill-rel-guid',$,$,$,#110,#112);
ENDSEC;
END-ISO-10303-21;
)ifc";

  const auto model = container::geometry::ifc::LoadFromStep(kIfc);

  ASSERT_EQ(model.elements.size(), 5u);
  const auto findElement = [&](std::string_view guid) {
    return std::ranges::find_if(model.elements, [&](const auto &element) {
      return element.guid == guid;
    });
  };
  const auto hasProperty = [](const auto &element, std::string_view set,
                              std::string_view name,
                              std::string_view category,
                              std::string_view value) {
    return std::ranges::any_of(element.properties, [&](const auto &property) {
      return property.set == set && property.name == name &&
             property.category == category && property.value == value;
    });
  };

  const auto wall = findElement("wall-guid");
  ASSERT_NE(wall, model.elements.end());
  EXPECT_EQ(wall->materialName, "Gypsum Board");
  EXPECT_TRUE(hasProperty(*wall, "IFCRELDEFINESBYTYPE", "Type",
                          "relationship", "Wall Type A"));
  EXPECT_TRUE(hasProperty(*wall, "Pset_TypeCommon", "TypeMark", "pset",
                          "WT-01"));
  EXPECT_TRUE(hasProperty(*wall, "IFCRELASSOCIATESMATERIAL", "Material",
                          "relationship", "Gypsum Board"));
  EXPECT_TRUE(hasProperty(*wall, "IFCMATERIALLAYERSETUSAGE", "LayerSet",
                          "material", "Wall Layer Set"));
  EXPECT_TRUE(hasProperty(*wall, "IFCMATERIALLAYERSET", "Layer.2", "material",
                          "Concrete"));
  EXPECT_TRUE(hasProperty(*wall, "IFCRELAGGREGATES", "Parent",
                          "relationship", "Assembly"));
  EXPECT_TRUE(hasProperty(*wall, "IFCRELNESTS", "Child.1", "relationship",
                          "Nested Part"));
  EXPECT_TRUE(hasProperty(*wall, "IFCRELVOIDSELEMENT", "Opening",
                          "relationship", "Window Opening"));
  EXPECT_TRUE(hasProperty(*wall, "IFCRELFILLSELEMENT", "FilledBy",
                          "relationship", "Window"));

  const auto beam = findElement("beam-guid");
  ASSERT_NE(beam, model.elements.end());
  EXPECT_TRUE(hasProperty(*beam, "IFCMATERIALPROFILESETUSAGE", "ProfileSet",
                          "material", "Beam Profile Set"));
  EXPECT_TRUE(hasProperty(*beam, "IFCMATERIALPROFILESET",
                          "Profile.1.Material", "material", "Steel"));
  EXPECT_TRUE(hasProperty(*beam, "IFCRELAGGREGATES", "Parent",
                          "relationship", "Assembly"));

  const auto assembly = findElement("assembly-guid");
  ASSERT_NE(assembly, model.elements.end());
  EXPECT_TRUE(hasProperty(*assembly, "IFCRELAGGREGATES", "Child.1",
                          "relationship", "Wall Instance"));

  const auto nested = findElement("nested-guid");
  ASSERT_NE(nested, model.elements.end());
  EXPECT_TRUE(hasProperty(*nested, "IFCRELNESTS", "Parent", "relationship",
                          "Wall Instance"));

  const auto window = findElement("window-guid");
  ASSERT_NE(window, model.elements.end());
  EXPECT_TRUE(hasProperty(*window, "IFCRELFILLSELEMENT", "Opening",
                          "relationship", "Window Opening"));
  EXPECT_TRUE(hasProperty(*window, "IFCRELFILLSELEMENT", "VoidsElement",
                          "relationship", "Wall Instance"));
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
