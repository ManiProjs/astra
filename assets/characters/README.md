# Character assets

- `AstraPlayer-runtime.glb`: open-world player used by the demo. It preserves the 69-joint rig, all three animations and materials, with embedded textures downsampled to a maximum of 512px for lower unified-memory use.
- `AstraPlayer.glb`: original higher-resolution export retained as the source-quality version. See `AstraPlayer-LICENSE.md` for licensing and motion-data attribution.
- `CesiumMan.glb`: compact glTF importer regression fixture, not the open-world player. See `CesiumMan-LICENSE.md`.
- `Boarbound-Human.glb`: a complete, textured 262K-triangle human character from Grab3D's Man Base Character model (CC0 1.0).
- `Boarbound.glb`: a separate three-part domestic boar character from 3DAssets.dev's Farm Animals and Barnyard set (CC0 1.0).
- Both are genuine imported GLB character assets, not meshes assembled from primitives. The transformation demo loads their real geometry and materials and blends between the two silhouettes.
