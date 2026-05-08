# BIM GPU Visibility And Draw Compaction

This note documents the BIM visibility and draw compaction changes that keep
large BIM scenes on the GPU while preserving semantic filters such as isolate,
hide, class, storey, material, phase, fire rating, load-bearing, status, and
draw budget.

## Runtime Flow

```text
RendererFrontend
  -> BimManager::updateVisibilityFilterSettings(filter)
  -> FrameRecorder::prepareDrawCompaction(...)
  -> BimManager::recordVisibilityFilterUpdate(...)
  -> BimManager::recordDrawCompactionUpdate(...)
  -> FrameRecorder BIM passes draw compacted indirect streams
```

The visibility filter compute pass writes one object visibility bit per BIM
object. The draw compaction compute pass reads that mask together with meshlet
residency and produces indirect draw buffers for each BIM draw bucket. The BIM
depth, G-buffer, transparent, pick, shadow, native point, and native curve paths
can then use indirect draws without rebuilding CPU filtered lists every frame.

## Safety Contracts

GPU compacted draws are allowed only when `visibilityMaskReadyForDrawCompaction`
returns true. That requires a resident mask buffer, valid object metadata, no
pending visibility dispatch, and a current mask. Active filters also require the
visibility compute pipeline and descriptor set to be available. If the shader is
missing or the pipeline cannot be created, this readiness check stays false and
the renderer falls back to CPU-filtered draw lists.

The all-visible mask is a valid ready state only when no semantic filter is
active. It is not treated as proof that active isolate, hide, class, storey, or
material filters were applied.

Frame recording may call `prepareDrawCompaction` each frame, but the method
caches the source draw-list pointer, size, and object-data revision. Unchanged
draw lists keep their GPU input buffer and only request a new compaction dispatch
when the visibility mask or residency output invalidates previous compacted
results.

## Performance Behavior

`updateVisibilityFilterSettings` avoids redispatching when settings are
unchanged and the current mask is still valid. In the common inactive-filter
case, the upload-time all-visible mask is reused instead of running a full
object-count compute pass every frame.

`prepareDrawCompaction` separates static input uploads from dynamic output
compaction. Source draw uploads are tied to draw-list identity and object-data
revision. Residency, LOD, or semantic filter changes invalidate only the
compacted output, so large models avoid repeated CPU-to-GPU draw-list uploads.

Native point and curve primitives use dedicated compaction slots. Their
residency metadata marks them as native primitives even when they have no
meshlet clusters, allowing them to share the same visibility and draw-budget
path as mesh-backed BIM geometry.

## Failure Modes

- Missing `bim_visibility_filter.comp.spv`: GPU filtering is unavailable for
  active filters, so CPU-filtered draw lists are used.
- Pending visibility dispatch: compacted draws are not considered ready until
  the dispatch runs and publishes the mask.
- Changed filter settings: previous compacted draw outputs are invalidated and
  regenerated from the new mask.
- Changed draw-list source or object-data revision: source indirect input is
  reuploaded before compaction.

## Verification

Focused coverage lives in `rendering_convention_tests`, including contracts for
visibility readiness, draw-list source caching, native point and curve
compaction slots, and CPU fallback routing. Full validation should also build
`VulkanSceneRenderer` so Slang shader compilation exercises the compute kernels.
