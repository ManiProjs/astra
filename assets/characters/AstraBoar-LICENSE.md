# AstraBoar asset source and license

- Geometry source: [Forest Boar, Jointed — FPS Survival Forest Outpost](https://3dassets.dev/assets/fps-survival-forest-outpost-creature-boar-2eae041a)
- Geometry source model: `https://cdn.3dassets.dev/assets/32710/v1/model.glb`
- Rig and animation source: [Gobkit Free Animal Pack Vol.14, `WildBoar.glb`](https://gobkit.com/freebies/animal012/WildBoar.glb)
- Local rig source: `source/Gobkit-WildBoar-rig.glb`
- Both sources are released under CC0 1.0 Universal (public domain dedication)
- Both contributors disclose AI-assisted asset creation
- Local derivative: `AstraBoar.glb`

The derivative uses the Forest Boar geometry, transfers vertex weights from the 16-bone Gobkit
WildBoar rig, remaps the rest skeleton to the detailed mesh bounds, and preserves the rig's
`idle`, `walk`, `attack`, and `dead` clips. The geometry is remeshed, subdivided, UV-unwrapped,
and given a generated hide texture. The exported GLB contains a 13-joint skin and roughly
966K triangles. Both source models are stylized and AI-assisted; remeshing and subdivision
increase density but do not create professionally sculpted anatomical detail. See
`AstraBoar-report.txt` for the generated mesh and weight-transfer measurements.
