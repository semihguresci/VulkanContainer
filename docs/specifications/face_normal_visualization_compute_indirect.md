# Face Normal Visualization via Compute + Draw Indirect

## 1. Purpose

Render one debug line per triangle face, where each line originates at the face centroid and extends in the direction of the face normal. This is implemented using a GPU-driven pipeline:

- Compute shader generates line geometry
- Indirect draw submits rendering without CPU-side geometry rebuild

This feature is used for:

- Mesh validation
- Winding verification
- Normal transformation debugging
- glTF import validation
- Coordinate system debugging

---

## 2. High-Level Design

### Pipeline Stages

1. Mesh triangle data resides on GPU
2. Compute shader processes triangles
3. Compute computes:
   - Face centroid
   - Face normal
   - Line endpoints
   - Debug color
4. Compute writes:
   - Line vertex buffer
   - Indirect draw arguments
5. Graphics pass renders line list using indirect draw

---

## 3. Functional Requirements

### Input

- Position buffer
- Optional index buffer
- Per-object model matrix
- Optional normal matrix

### Output

For each triangle:

- 2 vertices (line segment)
- Optional color
- Indirect draw command

---

## 4. Mathematical Definition

### Face Centroid
C = (v0 + v1 + v2) / 3

### Face Normal
N = normalize(cross(v1 - v0, v2 - v0))

### World Transform
C_world = M * float4(C, 1.0)
N_world = normalize((M^-1)^T * N)

### Line Endpoints
P0 = C_world
P1 = C_world + N_world * normalScale

---

## 5. Summary

Face Normal Visualization via Compute + Draw Indirect is a GPU-driven debug rendering technique that generates one line segment per triangle face in a compute pass. Each line originates at the face centroid and extends along the transformed geometric face normal. The generated geometry is rendered using indirect drawing, enabling scalable and efficient validation of mesh orientation, normal correctness, and transformation integrity.
