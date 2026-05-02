# Player and Editor UX Plan

This plan defines the user experience direction for splitting the renderer into
two primary modes: a focused player/renderer mode and a full editor mode. The
goal is to grow the project toward a professional scene creation tool inspired
by Blender, Maya, Revit, Unreal Engine, and AI-assisted workflows without
copying their complexity directly.

## Product Direction

The application should be built around one shared scene engine with two
different workspaces on top of it:

- Player / Renderer Mode: view, present, interact with, and render scenes.
- Editor Mode: create, organize, modify, inspect, and save scenes.

Player mode should feel confident and minimal. Editor mode should feel powerful
and inspectable. AI should accelerate editing, but every AI action should remain
previewable, reversible, and understandable.

## UX Principles

- Keep the current task obvious: the user should always know whether they are
  viewing the scene or editing it.
- Make everything selectable, inspectable, editable, reusable, and undoable.
- Separate navigation, selection, transformation, rendering, and AI actions so
  they do not fight for the same input.
- Prefer a simple default workspace with optional professional panels instead
  of showing every advanced feature at once.
- Treat AI output as proposed scene changes, not hidden automatic edits.
- Preserve renderer-engineering workflows such as debug views and performance
  inspection, but avoid exposing them as the main user path.

## Mode Model

### Player / Renderer Mode

Player mode is for consuming the scene.

Expected UX:

- Minimal chrome around the viewport.
- Camera navigation, scene navigation, and playback controls.
- Render-quality controls such as realtime, preview, and final render.
- Screenshot, recording, and render/export commands.
- Optional scene bookmarks or camera paths.
- Optional overlays for debug, statistics, or selected presentation metadata.
- No accidental editing of scene objects.

Useful controls:

- Play / pause scene simulation or animation.
- Switch active camera.
- Toggle walkthrough, orbit, and cinematic navigation.
- Adjust quality presets.
- Export still image, animation, or frame capture.
- Return to editor mode at the same camera position and selection context.

### Editor Mode

Editor mode is for authoring the scene.

Expected UX:

- A central viewport with professional 3D editing controls.
- A scene hierarchy for selecting and organizing objects.
- An inspector for transforms, materials, renderer settings, metadata, and
  custom properties.
- An asset browser for models, materials, textures, cameras, lights, prefabs,
  subscenes, and AI-generated assets.
- Toolbars for transform tools, snapping, coordinate space, pivots, overlays,
  and selection modes.
- Undo/redo and visible history for user and AI edits.
- A contextual AI assistant that can modify the current selection, create new
  scene content, organize assets, and propose improvements.

Editor mode should support direct manipulation and numeric precision. The user
should be able to drag a gizmo, type exact values, or ask AI to propose a
change.

## Recommended Workspace Layout

The default editor layout should be:

```text
Top Bar
[Project] [Scene] [Add] [Tools] [Render] [AI]     Mode: Editor / Player

Left Panel
Scene Hierarchy:
- World
  - Scene A
  - Building
  - Character
  - Lights
  - Cameras

Center
3D Viewport:
- selection
- transform gizmos
- snapping overlays
- camera navigation
- debug overlays

Right Panel
Inspector:
- transform
- material
- light/camera settings
- renderer settings
- BIM/Revit-style metadata
- custom properties

Bottom Panel
Assets / Timeline / Console / History

AI Panel
Prompt input, proposed changes, previews, and apply/reject controls
```

This layout gives the user a familiar professional editor shape while keeping
the first screen focused on the actual scene, not a landing page or wizard.

## Scene Authoring Model

Use a scene model that supports nested, reusable content.

- Scene: a complete editable world or level.
- Subscene: a scene nested inside another scene.
- Prefab / Blueprint: a reusable object group with stable defaults.
- Instance: a placed copy of a prefab or subscene with local overrides.
- Component: a behavior, render property, metadata block, or capability
  attached to an object.

This supports workflows similar to:

- Unreal Engine levels and blueprints.
- Unity prefabs.
- Blender collections.
- Revit families and building metadata.
- Maya grouped assets and animation rigs.

The user should be able to add a subscene to another scene, select it as a
single object, expand it in the hierarchy, override local properties, and later
open it for deeper editing.

## Core Editor Workflows

### Add Content

The Add menu should support:

- Scene
- Subscene
- Empty object
- Mesh/model
- BIM/imported building model
- Light
- Camera
- Material
- Environment
- Animation/camera path
- AI-generated object or scene

Adding content should create a selected object, place it predictably, and show
its editable properties immediately in the inspector.

### Select and Inspect

Selection should work from both the viewport and hierarchy.

- Single click selects.
- Double click frames the object.
- Multi-select supports group operations.
- The inspector shows the combined editable properties for the selection.
- Metadata should be searchable and usable for BIM/Revit-style workflows.

### Transform and Organize

Editor mode should provide:

- Move, rotate, and scale gizmos.
- Local/world transform spaces.
- Pivot controls.
- Grid, angle, and surface snapping.
- Duplicate, group, parent, unparent, hide, lock, and rename.
- Layers, collections, or tags for large-scene organization.

### Test in Player Mode

Switching to player mode should be immediate.

- Keep camera position when switching.
- Preserve selected scene context when returning to editor mode.
- Prevent editing while in player mode.
- Make it easy to test lighting, navigation, animation, and final rendering.

## AI Assistant UX

AI should behave like a contextual editor assistant, not just a chat window.

Example workflow:

```text
User prompt:
"Add a modern glass office tower next to this building."

AI proposes:
+ New object group: Office_Tower_01
+ Materials: glass, steel, concrete
+ Lights adjusted for reflections
+ Camera marker added

Actions:
[Preview] [Apply] [Edit Prompt] [Reject]
```

AI edits should be represented as structured proposed changes:

- Created objects
- Deleted objects
- Modified properties
- Generated materials
- Imported assets
- Scene hierarchy changes
- Lighting/render setting changes
- Metadata changes

The user should be able to preview the result, inspect the diff, apply it,
reject it, or revise the prompt. Applied AI changes should become normal undo
steps in the history.

Useful AI commands:

- Generate object or scene.
- Modify selected object.
- Explain selected object.
- Optimize scene.
- Improve realism.
- Convert blockout/sketch content into detailed scene content.
- Suggest lighting.
- Find problems.
- Create a Revit-like floor plan from a model.
- Generate or assign materials.
- Batch rename or organize the hierarchy.
- Create animation or camera paths.

## Workspace Presets

Start with one simple editor layout, then add workspace presets as features
grow:

- Layout: general scene arrangement and hierarchy work.
- Modeling: mesh and object editing.
- Architecture: BIM, Revit-like metadata, floors, rooms, and measurements.
- Lighting: lights, environment, exposure, and render previews.
- Animation: timeline, keyframes, cameras, and playback.
- Simulation: physics, interactive state, and runtime behavior.
- Rendering: render graph, quality, passes, and export.
- AI Assist: prompt history, generated assets, proposals, and scene analysis.

Workspace presets should rearrange tools without changing the underlying scene
or hiding essential mode controls.

## Implementation Phases

### Phase 1: Mode Foundation

- Add explicit Editor and Player modes.
- Add a visible mode switch.
- Define input ownership for mode-specific controls.
- Preserve camera state across mode switches.
- Disable object editing in player mode.

### Phase 2: Basic Editor Shell

- Add scene hierarchy panel.
- Add object selection.
- Add inspector panel for transform and basic render properties.
- Add Add menu for lights, cameras, imported models, and empty objects.
- Add undo/redo for editor actions.

### Phase 3: Scene Composition

- Add subscene and prefab concepts.
- Add instancing and local overrides.
- Add hierarchy organization tools.
- Add save/load support for editor-authored scene composition.

### Phase 4: Professional Tools

- Add transform gizmos.
- Add snapping and measurement tools.
- Add material editing.
- Add camera path and timeline tools.
- Add workspace presets.

### Phase 5: AI-Assisted Editing

- Add contextual AI panel.
- Represent AI output as structured proposed scene changes.
- Add preview/apply/reject workflow.
- Add AI changes to undo/redo history.
- Add scene analysis, cleanup, and optimization commands.

## Success Criteria

- A new user can open the app, load a scene, switch between player and editor
  modes, add content, select it, inspect it, modify it, and return to player
  mode without losing context.
- A technical user can still access renderer debugging and performance tools.
- Large scenes remain manageable through hierarchy, search, organization,
  metadata, and subscene composition.
- AI accelerates authoring without reducing user control or making silent,
  destructive changes.
