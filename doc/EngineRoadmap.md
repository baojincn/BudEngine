# Engine Roadmap

This document is the top-level milestone roadmap for BudEngine across rendering, physics, animation, audio, and runtime integration.

## Roadmap Scope

- Rendering (primary active track)
- Physics
- Animation
- Audio
- Cross-system runtime integration

## Milestone Timeline (Draft v1)

| Milestone | Focus | Target Outcome | Status |
|---|---|---|---|
| M0 | Foundation | Stable `BudEngine` orchestration (`Engine`/`RHI`/`Renderer` boundaries) | Done |
| M1 | Rendering Stage 1 | CPU macro-culling (LBVH-oriented frustum filtering & screen-area heuristic) | Done |
| M1.5| Asset & Shader Toolchain| `cgltf` scene parsing, file-watcher hot-reload, and DXC (HLSL->SPIR-V) compiler pipeline | Planned |
| M2 | Rendering Stage 2 | GPU instance culling (`Z-Prepass` + `Hi-Z` + occlusion compaction) | Planned |
| M3 | Rendering Stage 3 | High-end meshlet/micro-culling path (Task/Mesh or Compute fallback) | Planned |
| M4 | Rendering Stage 4 | Indirect draw / optional visibility-buffer dispatch path | Planned |
| M5 | Rendering Stage 5 | Neural rendering path (AI denoise + in-house CNN neural super-resolution) | Planned |
| M6 | Cross-system Vertical Slice | Integrate rendering + physics + animation + audio in one playable sample | Planned |
| M7 | Runtime and Tools | Blender Live Link bridge (Socket-based), asset dependency tracking, in-engine debug panels | Planned |
| M8 | Physics and Animation Upgrade | Constraints, retargeting, IK baseline, and gameplay-ready character stack | Planned |
| M9 | Audio and Platform Integration | Spatial audio, bus/mixer policy, input mapping, and save/load versioning | Planned |
| M10 | Production Hardening | Automated regression tests, performance gates, and crash replay package pipeline | Planned |

## RenderingRoadmap

Rendering is implemented as a scalable multi-profile pipeline:

- **Mobile/TBDR profile**: CPU Frustum + optional Software Occlusion Culling + direct/low-cost draw path.
- **Mainstream profile**: Compute-based GPU culling (`Z-Prepass`, `Hi-Z`, occlusion, indirect draw).
- **High-end profile**: Task/Mesh (or Compute expansion) + meshlet-level culling + neural rendering.

### Rendering Milestones

- **R1 (Done):** CPU macro-culling at instance level.
- **R1.5:** Unified HLSL shader compilation pipeline & glTF data ingestion.
- **R2:** GPU instance-level occlusion culling pipeline.
- **R3:** Meshlet-level fine-grained culling pipeline.
- **R4:** Low-resolution internal raster + robust motion vector output.
- **R5:** Neural upscaling/denoise integration before present.

### Milestone Definition of Done (DoD) Snapshots

- **M1.5 / R1.5 (Asset and Shader Toolchain)**
  - `cgltf` scene load path can import at least one representative `.gltf` sample end-to-end.
  - File watcher detects changed assets and triggers hot-reload without restarting runtime.
  - DXC-based HLSL-to-SPIR-V compilation is integrated in build/runtime workflow with clear error reporting.

- **R2 (GPU Instance Culling)**
  - `Z-Prepass + Hi-Z + occlusion compaction` path is functional and can be toggled for A/B comparison.
  - GPU culling path produces deterministic visible-instance counts across repeated runs in same camera state.
  - Frame-time or GPU-pass metrics are captured before/after enabling R2 for baseline comparison.

- **R5 (Neural Rendering / Super-resolution)**
  - In-house CNN super-resolution pass is integrated as a stable post-process compute stage.
  - Runtime path supports required inputs (`low_res_color`, `depth`, `motion_vectors`, optional history).
  - Quality/performance gates are reported (`PSNR/SSIM` offline, frame-time budget online).

- **L0 (Blender File-based Hot Reload Foundation)**
  - Blender-exported `.gltf` modifications appear in BudEngine through file-based hot reload.
  - Transform/hierarchy updates can be applied interactively without process restart.
  - Failed asset reload keeps previous valid runtime state (safe fallback behavior).

### Rendering Extension Tracks (Planned)

- **Shader & Pipeline Track (Crucial Infrastructure)**
  - HLSL as the single source of truth for all shader stages.
  - Runtime/Offline DXC integration (HLSL to SPIR-V).
  - Automated SPIR-V reflection to generate C++ Descriptor Set Layouts.

- **Geometry Track**
  - GPU-driven culling completion (`Hi-Z` occlusion in compute path).
  - Meshlet and virtualized geometry progression for high-end profile.
  - GPU compute skinning path for animation-heavy scenes.

- **Lighting and Shadows Track**
  - Clustered Forward/Deferred lighting for high dynamic light counts.
  - Virtual shadow map style page-based shadow strategy exploration.

- **Global Illumination Track**
  - SSGI baseline pass for cost-effective diffuse bounce approximation.
  - DDGI probe-based realtime GI track (hardware RT when available, software fallback otherwise).

- **Post-Processing Track**
  - TAA pipeline using history and motion vectors.
  - Physical camera stack (`bloom`, `depth of field`, `auto exposure`).
  - In-house CNN neural super-resolution as primary path, with optional external upscaling backend adapters where platform support allows.

See `doc/Graphics.md` for pass-level data flow and implementation details.

## Blender Live Link Roadmap (External Editor Strategy)

BudEngine uses Blender as the primary external content editor to avoid building a heavy in-engine editor stack too early and keep engineering focus on core runtime/rendering technology.

### Design Principles

- Use Blender as DCC authoring front-end, BudEngine as runtime/back-end.
- Keep `Renderer` and `RHI` unaware of Blender protocol details.
- Process all editor updates through runtime scene/tooling bridge layers.
- Keep render submit/present ownership on render thread unchanged.

### Live Link Phases

- **L0: File-Based Hot Reloading (The Foundation)**
  - Integrate `cgltf` to parse `.gltf` scenes as the Single Source of Truth.
  - OS-level file watcher (e.g., `std::filesystem` last write time) to detect Blender exports.
  - Instant runtime scene reconstruction (Transform/Hierarchy updates) upon file change.
- **L1:** One-way socket session from Blender to BudEngine using local transport (127.0.0.1).
- **L2:** Incremental scene patch sync (`create`, `update`, `delete`) with stable object UUID mapping.
- **L3:** Real-time lightweight updates (transform/light/camera/material scalar parameters).
- **L4:** Asynchronous heavy asset refresh (mesh/skeleton/animation/texture) with safe hot-reload fallback.
- **L5:** Session reliability (throttling, batching, reconnect, full-scene resync).
- **L6:** Optional two-way sync for selected runtime-authored metadata.

### Minimal Data Contract (v1)

- Stable object/entity identity (`uuid`).
- Operation type (`create`, `update`, `delete`).
- Component channel (`transform`, `mesh`, `material`, `light`, `camera`, `animation`).
- Revision ordering field (`revision`) for deterministic patch application.

### Acceptance Gates

- Scene transform edits in Blender are visible in BudEngine within interactive latency budget.
- Heavy asset import does not stall frame loop and preserves previous valid resource on failure.
- Live Link disconnect/reconnect can recover to a deterministic scene state.

## Physics Roadmap (Planned)

- **P1:** Basic rigid-body world lifecycle + collision broadphase/narrowphase baseline.
- **P2:** Character controller and scene query API.
- **P3:** Deterministic stepping policy and debug visualization.

## Animation Roadmap (Planned)

- **A1:** Skeleton import/evaluation baseline.
- **A2:** Blend tree/state machine foundation.
- **A3:** Runtime animation events and root motion support.

## Audio Roadmap (Planned)

- **AU1:** Core audio device/mixer abstraction.
- **AU2:** Spatial audio and listener pipeline.
- **AU3:** Streaming/voice management and profiling hooks.

## Feature Backlog (Cross-domain)

### Rendering
- FrameGraph visualization and pass/barrier/resource lifetime inspection.
- GPU timestamps and pipeline statistics capture for regression baselines.
- Dynamic resolution scaling for stable frame time before neural upscaling.
- Extended PBR material features (`clear coat`, `anisotropy`, `transmission`).
- Compute-based GPU skinning pipeline and animation-to-render binding optimization.
- Clustered lighting light-list build and per-cluster debug visualization.
- Virtual shadow map page management prototype and residency diagnostics.
- SSGI prototype pass with temporal stabilization path.
- DDGI probe update scheduling and runtime debug volume visualization.
- TAA resolve pass with history rejection and ghosting control.
- Physical camera post stack (`bloom`, `depth of field`, `auto exposure`) as independent graph passes.
- Upscaling integration abstraction for temporal super-resolution backends.
- In-house CNN super-resolution pipeline (`dataset capture`, `training/export`, `runtime inference integration`).
- Neural quality/performance validation harness (`PSNR/SSIM`, temporal stability, frame-time budget gates).

### Runtime and Tools
- Full hot-reload chain (`shader`, `material`, `scene`).
- Asset dependency graph and incremental asset build pipeline.
- Crash replay package generation (scene/config/log/GPU marker snapshot).
- In-engine debug panel for render/physics/audio toggles and stats.
- Blender external-editor Live Link bridge (`Blender Add-on` -> `BudEngine Runtime Bridge`).
- Incremental patch protocol with stable UUID mapping and ordered revision apply.

### Physics
- Continuous collision detection (`CCD`).
- Constraint system (`hinge`, `slider`, `6DoF`).
- Physics material policy (friction/restitution combine modes).
- Fixed-step and rollback-friendly simulation policy.

### Animation
- Skeleton retargeting across character rigs.
- IK baseline (two-bone IK first, then full-body extension).
- Animation compression and streaming strategy.
- Animation event track for VFX/audio/gameplay triggers.

### Audio
- Reverb zones and occlusion/obstruction baseline.
- Bus/mixer hierarchy with side-chain ducking support.
- Streamed BGM + preloaded SFX policy.
- Audio diagnostics (voice count, underrun watchdog, frame cost).

### Platform and Engine Layer
- Action/axis input mapping with runtime rebinding.
- Save/load system with versioned data migration.
- Quality tier/profile switching for platform scaling.
- Automated image-based render regression tests and performance threshold checks.

## Neural Rendering and Simulation Vision (Long-term)

The following items are tracked as long-term research directions. They are not immediate milestone blockers and should be evaluated behind clear quality/performance gates.

### Near-term research candidates
- AI denoising for realtime ray tracing (`1 spp` + `depth/normal/motion vectors` as network inputs).
- In-house temporal neural super-resolution quality iteration and stability improvements.

### Mid-term research candidates
- Neural Radiance Cache (`NRC`) as a dynamic GI acceleration strategy.
- Neural Texture Compression (`NTC`) for high-resolution material memory reduction.

### Long-term experimental track
- Hybrid neural primitives (`3DGS`/`NeRF`) blended with triangle-based rendering by depth-consistent composition.
- Neural animation and physics surrogates (learned motion matching and data-driven cloth/fluid approximations).

### Research execution policy
- Keep production path stable first; shipable rendering path has priority over experimental branches.
- All neural features must include reproducible metrics (quality, temporal stability, frame-time cost, memory cost).
- Experimental modules should be isolated to allow fallback to deterministic non-neural paths.

## Execution Rules

- Keep render submit/present on the render thread.
- Maintain strict `Engine`/`RHI`/`Renderer` responsibility boundaries.
- Use milestone completion gates with measurable checklists before promoting to next milestone.
