#include "Container/renderer/bim/BimSectionCapBuilder.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <unordered_map>

namespace container::renderer {

[[nodiscard]] glm::vec4 normalizedSectionCapPlane(glm::vec4 plane) {
  const glm::vec3 normal{plane};
  const float length = glm::length(normal);
  if (!std::isfinite(length) || length <= 1.0e-6f) {
    return {0.0f, 1.0f, 0.0f, 0.0f};
  }
  return plane / length;
}

[[nodiscard]] BimSectionCapGeneratedMesh BimSectionCapBuilder::build(
    std::span<const BimSectionCapTriangle> triangles,
    const BimSectionCapBuildOptions& options) const {
  struct Segment {
    uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
    glm::vec3 a{0.0f};
    glm::vec3 b{0.0f};
  };

  constexpr float kDistanceEpsilon = 1.0e-5f;
  constexpr float kPointMergeEpsilon2 = 1.0e-10f;
  constexpr float kMinSegmentLength2 = 1.0e-8f;

  BimSectionCapGeneratedMesh mesh{};
  std::array<glm::vec4, kBimSectionCapMaxPlanes> clipPlanes{};
  uint32_t clipPlaneCount =
      std::min<uint32_t>(options.clipPlaneCount,
                         static_cast<uint32_t>(clipPlanes.size()));
  if (clipPlaneCount == 0u) {
    clipPlaneCount = 1u;
    clipPlanes[0] = normalizedSectionCapPlane(options.sectionPlane);
  } else {
    for (uint32_t planeIndex = 0; planeIndex < clipPlaneCount; ++planeIndex) {
      clipPlanes[planeIndex] =
          normalizedSectionCapPlane(options.clipPlanes[planeIndex]);
    }
  }

  const float spacing =
      std::max(std::isfinite(options.hatchSpacing) ? options.hatchSpacing
                                                   : 0.25f,
               0.001f);
  const float hatchAngle =
      std::isfinite(options.hatchAngleRadians) ? options.hatchAngleRadians
                                               : 0.7853982f;

  auto finitePoint = [](const glm::vec3& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) &&
           std::isfinite(point.z);
  };

  auto addUniquePoint = [](std::vector<glm::vec3>& points,
                           const glm::vec3& point) {
    for (const glm::vec3& existing : points) {
      if (glm::dot(existing - point, existing - point) <=
          kPointMergeEpsilon2) {
        return;
      }
    }
    points.push_back(point);
  };

  auto signedDistanceToPlane = [](const glm::vec4& plane,
                                  const glm::vec3& point) {
    return glm::dot(glm::vec3{plane}, point) + plane.w;
  };

  auto clipPolygonToPlane = [&](const std::vector<glm::vec3>& polygon,
                                const glm::vec4& plane) {
    std::vector<glm::vec3> clipped;
    if (polygon.empty()) {
      return clipped;
    }

    auto appendPoint = [&](const glm::vec3& point) {
      if (clipped.empty() ||
          glm::dot(clipped.back() - point, clipped.back() - point) >
              kPointMergeEpsilon2) {
        clipped.push_back(point);
      }
    };

    glm::vec3 previous = polygon.back();
    float previousDistance = signedDistanceToPlane(plane, previous);
    bool previousInside = previousDistance >= -kDistanceEpsilon;
    for (const glm::vec3& current : polygon) {
      const float currentDistance = signedDistanceToPlane(plane, current);
      const bool currentInside = currentDistance >= -kDistanceEpsilon;
      if (previousInside != currentInside) {
        const float denominator = previousDistance - currentDistance;
        if (std::abs(denominator) > kDistanceEpsilon) {
          const float t = previousDistance / denominator;
          appendPoint(previous + (current - previous) * t);
        }
      }
      if (currentInside) {
        appendPoint(current);
      }
      previous = current;
      previousDistance = currentDistance;
      previousInside = currentInside;
    }
    if (clipped.size() > 1u &&
        glm::dot(clipped.front() - clipped.back(),
                 clipped.front() - clipped.back()) <= kPointMergeEpsilon2) {
      clipped.pop_back();
    }
    return clipped;
  };

  for (uint32_t capPlaneIndex = 0; capPlaneIndex < clipPlaneCount;
       ++capPlaneIndex) {
    const glm::vec4 plane = clipPlanes[capPlaneIndex];
    const glm::vec3 normal{plane};
    const glm::vec3 basisSeed =
        std::abs(normal.y) < 0.9f ? glm::vec3{0.0f, 1.0f, 0.0f}
                                  : glm::vec3{1.0f, 0.0f, 0.0f};
    const glm::vec3 axisU = glm::normalize(glm::cross(basisSeed, normal));
    const glm::vec3 axisV = glm::normalize(glm::cross(normal, axisU));
    const glm::vec3 planeOrigin = -plane.w * normal;

    std::vector<Segment> segments;
    segments.reserve(triangles.size());

    for (const BimSectionCapTriangle& triangle : triangles) {
      if (triangle.objectIndex == std::numeric_limits<uint32_t>::max() ||
          !finitePoint(triangle.p0) || !finitePoint(triangle.p1) ||
          !finitePoint(triangle.p2)) {
        continue;
      }

      const std::array<glm::vec3, 3> points{
          triangle.p0, triangle.p1, triangle.p2};
      const std::array<float, 3> distances{
          signedDistanceToPlane(plane, points[0]),
          signedDistanceToPlane(plane, points[1]),
          signedDistanceToPlane(plane, points[2])};
      const bool hasPositive =
          distances[0] > kDistanceEpsilon ||
          distances[1] > kDistanceEpsilon ||
          distances[2] > kDistanceEpsilon;
      const bool hasNegative =
          distances[0] < -kDistanceEpsilon ||
          distances[1] < -kDistanceEpsilon ||
          distances[2] < -kDistanceEpsilon;
      if (!hasPositive || !hasNegative) {
        continue;
      }

      std::vector<glm::vec3> crossingPoints;
      crossingPoints.reserve(2u);
      for (uint32_t edge = 0; edge < 3u; ++edge) {
        const uint32_t next = (edge + 1u) % 3u;
        const float d0 = distances[edge];
        const float d1 = distances[next];
        if (std::abs(d0) <= kDistanceEpsilon) {
          addUniquePoint(crossingPoints, points[edge]);
        }
        if ((d0 > kDistanceEpsilon && d1 < -kDistanceEpsilon) ||
            (d0 < -kDistanceEpsilon && d1 > kDistanceEpsilon)) {
          const float t = d0 / (d0 - d1);
          addUniquePoint(crossingPoints,
                         points[edge] + (points[next] - points[edge]) * t);
        }
        if (std::abs(d1) <= kDistanceEpsilon) {
          addUniquePoint(crossingPoints, points[next]);
        }
      }
      if (crossingPoints.size() != 2u) {
        continue;
      }

      if (glm::dot(crossingPoints[0] - crossingPoints[1],
                   crossingPoints[0] - crossingPoints[1]) <=
          kMinSegmentLength2) {
        continue;
      }
      segments.push_back(
          Segment{triangle.objectIndex, crossingPoints[0], crossingPoints[1]});
    }

    if (segments.empty()) {
      continue;
    }

    std::unordered_map<uint32_t, std::vector<Segment>> segmentsByObject;
    segmentsByObject.reserve(segments.size());
    for (const Segment& segment : segments) {
      segmentsByObject[segment.objectIndex].push_back(segment);
    }

    auto addVertex = [&](uint32_t objectIndex,
                         const glm::vec3& position) -> uint32_t {
      (void)objectIndex;
      container::geometry::Vertex vertex{};
      vertex.position = position + normal * options.capOffset;
      vertex.normal = normal;
      vertex.color = {1.0f, 1.0f, 1.0f};
      const uint32_t index = static_cast<uint32_t>(std::min<size_t>(
          mesh.vertices.size(), std::numeric_limits<uint32_t>::max()));
      mesh.vertices.push_back(vertex);
      return index;
    };

    auto toPlane2 = [&](const glm::vec3& point) {
      const glm::vec3 relative = point - planeOrigin;
      return glm::vec2{glm::dot(relative, axisU), glm::dot(relative, axisV)};
    };
    auto toPlane3 = [&](const glm::vec2& point) {
      return planeOrigin + axisU * point.x + axisV * point.y;
    };

    auto appendHatchesForTriangle =
        [&](uint32_t objectIndex, const glm::vec3& a, const glm::vec3& b,
            const glm::vec3& c, float angle) {
          const std::array<glm::vec2, 3> p{toPlane2(a), toPlane2(b),
                                           toPlane2(c)};
          const glm::vec2 direction{std::cos(angle), std::sin(angle)};
          const glm::vec2 lineNormal{-direction.y, direction.x};
          const std::array<float, 3> offsets{
              glm::dot(lineNormal, p[0]), glm::dot(lineNormal, p[1]),
              glm::dot(lineNormal, p[2])};
          const float minOffset =
              std::min({offsets[0], offsets[1], offsets[2]});
          const float maxOffset =
              std::max({offsets[0], offsets[1], offsets[2]});
          const int64_t firstLine =
              static_cast<int64_t>(std::ceil(minOffset / spacing));
          const int64_t lastLine =
              static_cast<int64_t>(std::floor(maxOffset / spacing));
          for (int64_t line = firstLine; line <= lastLine; ++line) {
            const float offset = static_cast<float>(line) * spacing;
            std::vector<glm::vec2> intersections;
            intersections.reserve(3u);
            for (uint32_t edge = 0; edge < 3u; ++edge) {
              const uint32_t next = (edge + 1u) % 3u;
              const float d0 = offsets[edge] - offset;
              const float d1 = offsets[next] - offset;
              if (std::abs(d0) <= kDistanceEpsilon &&
                  std::abs(d1) <= kDistanceEpsilon) {
                continue;
              }
              if ((d0 < -kDistanceEpsilon && d1 < -kDistanceEpsilon) ||
                  (d0 > kDistanceEpsilon && d1 > kDistanceEpsilon)) {
                continue;
              }
              const float denominator = d0 - d1;
              if (std::abs(denominator) <= kDistanceEpsilon) {
                continue;
              }
              const float t = std::clamp(d0 / denominator, 0.0f, 1.0f);
              const glm::vec2 point = p[edge] + (p[next] - p[edge]) * t;
              bool duplicate = false;
              for (const glm::vec2& existing : intersections) {
                if (glm::dot(existing - point, existing - point) <=
                    kPointMergeEpsilon2) {
                  duplicate = true;
                  break;
                }
              }
              if (!duplicate) {
                intersections.push_back(point);
              }
            }
            if (intersections.size() < 2u) {
              continue;
            }
            std::ranges::sort(intersections, [&](const glm::vec2& lhs,
                                                 const glm::vec2& rhs) {
              return glm::dot(direction, lhs) < glm::dot(direction, rhs);
            });
            const glm::vec3 h0 = toPlane3(intersections.front());
            const glm::vec3 h1 = toPlane3(intersections.back());
            if (glm::dot(h0 - h1, h0 - h1) <= kMinSegmentLength2) {
              continue;
            }
            const uint32_t i0 = addVertex(objectIndex, h0);
            const uint32_t i1 = addVertex(objectIndex, h1);
            mesh.indices.push_back(i0);
            mesh.indices.push_back(i1);
          }
        };

    auto reconstructSegmentLoops =
        [&](const std::vector<Segment>& objectSegments) {
          struct LoopEdge {
            uint32_t a{0};
            uint32_t b{0};
            bool used{false};
          };

          std::vector<glm::vec3> points;
          std::vector<LoopEdge> edges;
          points.reserve(objectSegments.size() * 2u);
          edges.reserve(objectSegments.size());

          auto pointIndexFor = [&](const glm::vec3& point) {
            for (uint32_t index = 0; index < points.size(); ++index) {
              if (glm::dot(points[index] - point, points[index] - point) <=
                  kPointMergeEpsilon2) {
                return index;
              }
            }
            const uint32_t index = static_cast<uint32_t>(std::min<size_t>(
                points.size(), std::numeric_limits<uint32_t>::max()));
            points.push_back(point);
            return index;
          };

          auto addEdge = [&](uint32_t a, uint32_t b) {
            if (a == b) {
              return;
            }
            const uint32_t lo = std::min(a, b);
            const uint32_t hi = std::max(a, b);
            for (const LoopEdge& edge : edges) {
              if (std::min(edge.a, edge.b) == lo &&
                  std::max(edge.a, edge.b) == hi) {
                return;
              }
            }
            edges.push_back(LoopEdge{a, b, false});
          };

          for (const Segment& segment : objectSegments) {
            if (glm::dot(segment.a - segment.b, segment.a - segment.b) <=
                kMinSegmentLength2) {
              continue;
            }
            addEdge(pointIndexFor(segment.a), pointIndexFor(segment.b));
          }

          std::vector<std::vector<glm::vec3>> loops;
          for (size_t edgeIndex = 0; edgeIndex < edges.size(); ++edgeIndex) {
            if (edges[edgeIndex].used) {
              continue;
            }

            edges[edgeIndex].used = true;
            std::vector<uint32_t> loop{edges[edgeIndex].a, edges[edgeIndex].b};
            uint32_t previous = edges[edgeIndex].a;
            uint32_t current = edges[edgeIndex].b;
            bool closed = false;

            for (size_t guard = 0; guard < edges.size(); ++guard) {
              size_t nextEdgeIndex = edges.size();
              uint32_t nextVertex = 0;
              for (size_t candidateIndex = 0; candidateIndex < edges.size();
                   ++candidateIndex) {
                if (edges[candidateIndex].used) {
                  continue;
                }
                const LoopEdge& candidate = edges[candidateIndex];
                if (candidate.a == current || candidate.b == current) {
                  const uint32_t other =
                      candidate.a == current ? candidate.b : candidate.a;
                  if (other == previous && other != loop.front()) {
                    continue;
                  }
                  nextEdgeIndex = candidateIndex;
                  nextVertex = other;
                  break;
                }
              }

              if (nextEdgeIndex == edges.size()) {
                break;
              }
              edges[nextEdgeIndex].used = true;
              if (nextVertex == loop.front()) {
                closed = true;
                break;
              }
              previous = current;
              current = nextVertex;
              loop.push_back(current);
            }

            if (!closed || loop.size() < 3u) {
              continue;
            }

            std::vector<glm::vec3> polygon;
            polygon.reserve(loop.size());
            for (uint32_t pointIndex : loop) {
              polygon.push_back(points[pointIndex]);
            }
            loops.push_back(std::move(polygon));
          }

          return loops;
        };

    auto polygonArea2 = [&](const std::vector<glm::vec3>& polygon) {
      if (polygon.size() < 3u) {
        return 0.0f;
      }
      float area = 0.0f;
      for (size_t i = 0; i < polygon.size(); ++i) {
        const glm::vec2 a = toPlane2(polygon[i]);
        const glm::vec2 b = toPlane2(polygon[(i + 1u) % polygon.size()]);
        area += a.x * b.y - b.x * a.y;
      }
      return area;
    };

    auto pointInTriangle2 = [&](const glm::vec2& point, const glm::vec2& a,
                                const glm::vec2& b, const glm::vec2& c) {
      const auto sign = [](const glm::vec2& p0, const glm::vec2& p1,
                           const glm::vec2& p2) {
        return (p0.x - p2.x) * (p1.y - p2.y) -
               (p1.x - p2.x) * (p0.y - p2.y);
      };
      const float d0 = sign(point, a, b);
      const float d1 = sign(point, b, c);
      const float d2 = sign(point, c, a);
      const bool hasNegative = d0 < -kDistanceEpsilon ||
                               d1 < -kDistanceEpsilon ||
                               d2 < -kDistanceEpsilon;
      const bool hasPositive = d0 > kDistanceEpsilon ||
                               d1 > kDistanceEpsilon ||
                               d2 > kDistanceEpsilon;
      return !(hasNegative && hasPositive);
    };

    auto pointInPolygon2 = [&](const glm::vec2& point,
                               const std::vector<glm::vec3>& polygon) {
      bool inside = false;
      for (size_t i = 0, j = polygon.size() - 1u; i < polygon.size(); j = i++) {
        const glm::vec2 a = toPlane2(polygon[i]);
        const glm::vec2 b = toPlane2(polygon[j]);
        const float denominator = b.y - a.y;
        if (std::abs(denominator) <= kDistanceEpsilon) {
          continue;
        }
        const bool intersects =
            ((a.y > point.y) != (b.y > point.y)) &&
            (point.x < (b.x - a.x) * (point.y - a.y) / denominator + a.x);
        if (intersects) {
          inside = !inside;
        }
      }
      return inside;
    };

    auto triangulatePolygon = [&](std::vector<glm::vec3> polygon) {
      std::vector<std::array<glm::vec3, 3>> trianglesOut;
      if (polygon.size() < 3u ||
          std::abs(polygonArea2(polygon)) <= kDistanceEpsilon) {
        return trianglesOut;
      }

      if (polygonArea2(polygon) < 0.0f) {
        std::ranges::reverse(polygon);
      }

      std::vector<uint32_t> remaining;
      remaining.reserve(polygon.size());
      for (uint32_t index = 0; index < polygon.size(); ++index) {
        remaining.push_back(index);
      }

      const auto cross2 = [&](uint32_t ia, uint32_t ib, uint32_t ic) {
        const glm::vec2 a = toPlane2(polygon[ia]);
        const glm::vec2 b = toPlane2(polygon[ib]);
        const glm::vec2 c = toPlane2(polygon[ic]);
        return (b.x - a.x) * (c.y - a.y) -
               (b.y - a.y) * (c.x - a.x);
      };

      size_t guard = polygon.size() * polygon.size();
      while (remaining.size() > 3u && guard-- > 0u) {
        bool clippedEar = false;
        for (size_t i = 0; i < remaining.size(); ++i) {
          const uint32_t ia =
              remaining[(i + remaining.size() - 1u) % remaining.size()];
          const uint32_t ib = remaining[i];
          const uint32_t ic = remaining[(i + 1u) % remaining.size()];
          if (cross2(ia, ib, ic) <= kDistanceEpsilon) {
            continue;
          }

          const glm::vec2 a = toPlane2(polygon[ia]);
          const glm::vec2 b = toPlane2(polygon[ib]);
          const glm::vec2 c = toPlane2(polygon[ic]);
          bool containsOtherPoint = false;
          for (uint32_t candidate : remaining) {
            if (candidate == ia || candidate == ib || candidate == ic) {
              continue;
            }
            if (pointInTriangle2(toPlane2(polygon[candidate]), a, b, c)) {
              containsOtherPoint = true;
              break;
            }
          }
          if (containsOtherPoint) {
            continue;
          }

          trianglesOut.push_back({polygon[ia], polygon[ib], polygon[ic]});
          remaining.erase(remaining.begin() +
                          static_cast<std::ptrdiff_t>(i));
          clippedEar = true;
          break;
        }

        if (!clippedEar) {
          trianglesOut.clear();
          break;
        }
      }

      if (remaining.size() == 3u) {
        trianglesOut.push_back({polygon[remaining[0]], polygon[remaining[1]],
                                polygon[remaining[2]]});
      } else if (trianglesOut.empty()) {
        for (size_t i = 1; i + 1u < polygon.size(); ++i) {
          trianglesOut.push_back({polygon[0], polygon[i], polygon[i + 1u]});
        }
      }
      return trianglesOut;
    };

    for (const auto& [objectIndex, objectSegments] : segmentsByObject) {
      std::vector<std::vector<glm::vec3>> polygons =
          reconstructSegmentLoops(objectSegments);
      if (polygons.empty()) {
        continue;
      }

      std::vector<std::array<glm::vec3, 3>> clippedTriangles;
      clippedTriangles.reserve(objectSegments.size());

      std::vector<std::vector<glm::vec3>> clippedPolygons;
      clippedPolygons.reserve(polygons.size());
      for (std::vector<glm::vec3> polygon : polygons) {
        for (uint32_t clipIndex = 0; clipIndex < clipPlaneCount; ++clipIndex) {
          if (clipIndex == capPlaneIndex) {
            continue;
          }
          polygon = clipPolygonToPlane(polygon, clipPlanes[clipIndex]);
          if (polygon.size() < 3u) {
            break;
          }
        }
        if (polygon.size() >= 3u &&
            std::abs(polygonArea2(polygon)) > kDistanceEpsilon) {
          clippedPolygons.push_back(std::move(polygon));
        }
      }

      const uint32_t fillFirstIndex = static_cast<uint32_t>(std::min<size_t>(
          mesh.indices.size(), std::numeric_limits<uint32_t>::max()));
      for (size_t polygonIndex = 0; polygonIndex < clippedPolygons.size();
           ++polygonIndex) {
        const std::vector<glm::vec3>& polygon = clippedPolygons[polygonIndex];
        glm::vec2 centroid{0.0f};
        for (const glm::vec3& point : polygon) {
          centroid += toPlane2(point);
        }
        centroid /= static_cast<float>(polygon.size());

        bool nestedHole = false;
        const float polygonArea = std::abs(polygonArea2(polygon));
        for (size_t otherIndex = 0; otherIndex < clippedPolygons.size();
             ++otherIndex) {
          if (otherIndex == polygonIndex) {
            continue;
          }
          const std::vector<glm::vec3>& other = clippedPolygons[otherIndex];
          if (std::abs(polygonArea2(other)) <= polygonArea) {
            continue;
          }
          if (pointInPolygon2(centroid, other)) {
            nestedHole = true;
            break;
          }
        }
        if (nestedHole) {
          continue;
        }

        const std::vector<std::array<glm::vec3, 3>> polygonTriangles =
            triangulatePolygon(polygon);
        for (const std::array<glm::vec3, 3>& triangle : polygonTriangles) {
          if (glm::dot(triangle[0] - triangle[1],
                       triangle[0] - triangle[1]) <=
                  kMinSegmentLength2 ||
              glm::dot(triangle[0] - triangle[2],
                       triangle[0] - triangle[2]) <= kMinSegmentLength2 ||
              glm::dot(triangle[1] - triangle[2],
                       triangle[1] - triangle[2]) <=
                  kMinSegmentLength2) {
            continue;
          }
          const uint32_t i0 = addVertex(objectIndex, triangle[0]);
          const uint32_t i1 = addVertex(objectIndex, triangle[1]);
          const uint32_t i2 = addVertex(objectIndex, triangle[2]);
          mesh.indices.push_back(i0);
          mesh.indices.push_back(i1);
          mesh.indices.push_back(i2);
          clippedTriangles.push_back(triangle);
        }
      }
      const uint32_t fillIndexCount =
          static_cast<uint32_t>(mesh.indices.size() - fillFirstIndex);
      if (fillIndexCount > 0u) {
        mesh.fillDrawCommands.push_back(DrawCommand{
            .objectIndex = objectIndex,
            .firstIndex = fillFirstIndex,
            .indexCount = fillIndexCount,
            .instanceCount = 1u,
        });
      }

      const uint32_t hatchFirstIndex = static_cast<uint32_t>(std::min<size_t>(
          mesh.indices.size(), std::numeric_limits<uint32_t>::max()));
      for (const std::array<glm::vec3, 3>& triangle : clippedTriangles) {
        appendHatchesForTriangle(objectIndex, triangle[0], triangle[1],
                                 triangle[2],
                                 hatchAngle);
        if (options.crossHatch) {
          appendHatchesForTriangle(objectIndex, triangle[0], triangle[1],
                                   triangle[2],
                                   hatchAngle + 1.57079632679f);
        }
      }
      const uint32_t hatchIndexCount =
          static_cast<uint32_t>(mesh.indices.size() - hatchFirstIndex);
      if (hatchIndexCount > 0u) {
        mesh.hatchDrawCommands.push_back(DrawCommand{
            .objectIndex = objectIndex,
            .firstIndex = hatchFirstIndex,
            .indexCount = hatchIndexCount,
            .instanceCount = 1u,
        });
      }
    }
  }

  return mesh;
}

[[nodiscard]] BimSectionCapGeneratedMesh BuildBimSectionCapMesh(
    std::span<const BimSectionCapTriangle> triangles,
    const BimSectionCapBuildOptions& options) {
  return BimSectionCapBuilder{}.build(triangles, options);
}

}  // namespace container::renderer
