# Character assets

- `AstraPlayer-runtime.glb`: open-world player used by the demo. It preserves the 69-joint rig, all three animations and materials, with embedded textures downsampled to a maximum of 512px for lower unified-memory use.
- `AstraPlayer.glb`: original higher-resolution export retained as the source-quality version. See `AstraPlayer-LICENSE.md` for licensing and motion-data attribution.
- `CesiumMan.glb`: compact glTF importer regression fixture, not the open-world player. See `CesiumMan-LICENSE.md`.
- `Boarbound-Human.glb`: a complete, textured 262K-triangle human character from Grab3D's Man Base Character model (CC0 1.0).
- `Boarbound.glb`: a separate three-part domestic boar character from 3DAssets.dev's Farm Animals and Barnyard set (CC0 1.0).
- `AstraBoar.glb`: a dense skinned derivative of 3DAssets.dev's Forest Boar geometry, rigged with Gobkit's 13-bone WildBoar skeleton and four animation clips. It has approximately 966K triangles, UVs, and a generated hide texture. See `AstraBoar-LICENSE.md` and `AstraBoar-report.txt`.
- `AstraBoarSkin.png`: generated 1024px hide texture used by `AstraBoar.glb`.
- `Boarbound-Human.glb` and `Boarbound.glb` remain source-asset inspection fixtures. They do not share
  topology or an authored correspondence. `AstraBoar.glb` is the current skinned boar runtime asset.
- A validated human-to-boar correspondence is still outstanding. An experimental nearest-surface
  projection produced severe folds and was discarded. The transformation demo does not use it.
