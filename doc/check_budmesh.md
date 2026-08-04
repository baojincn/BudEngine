BudMesh Quick Check
===================

This document describes the quick validation script for `.budmesh` files included in the repository.

File
- `check_budmesh.py` — located at project root. Use to inspect header, material table, texture list and a few meshlet entries.

Usage
- Run from project root with Python 3:
  - `python check_budmesh.py data/meshlets/Cube.budmesh`
  - `python check_budmesh.py data/meshlets/sponza.budmesh 10`  (prints first 10 meshlets)

Output summary
- Header fields: magic, version, counts, AABB, offsets.
- Materials table: `base_color_texture`, `alpha_mode`, `double_sided`, `alpha_cutoff`.
  - `alpha_mode` values: `0=OPAQUE`, `1=MASK`, `2=BLEND`.
- Textures: list of texture path strings written into the `.budmesh`.
- Meshlets: first N `MeshletDescriptor` and `MeshletCullData` entries.

Notes
- The script is intended for quick validation during asset processing and testing. It is not a full verifier.
- When the `BudMeshHeader` layout changes, update the script's unpack format and bump `MESH_VERSION` in `src/core/bud.asset.types.hpp`.
- For full glTF semantic compatibility and renderer integration, see the `tools/BudAssetTool` implementation and the engine loader/renderer code.

License
- Same license as repository.
