# Astra Player asset and animation credits

`AstraPlayer.glb` is a generated, high-detail human character assembled from MakeHuman core character assets. The MakeHuman core graphics assets (base mesh, skeleton, skin, eyes, hair and clothing) were released under **CC0 1.0 Universal**.

`AstraPlayer-runtime.glb` is a derived runtime copy of the same asset. It retains the original geometry, rig, materials and animation clips; embedded images larger than 512px were downsampled to 512px to reduce GPU memory. The original export remains available as `AstraPlayer.glb`.

MakeHuman licensing: https://www.makehumancommunity.org/about/license.html
CC0: https://creativecommons.org/publicdomain/zero/1.0/

The GLB contains only the `Idle`, `Walk`, and `Run` clips. The walking and running motion data is retargeted from the CMU Graphics Lab Motion Capture Database. The database permits use in products, including commercial products, but the motion data must not be resold directly, including in converted form. Acknowledgement:

> The data used in this project was obtained from mocap.cs.cmu.edu. The database was created with funding from NSF EIA-0196217.

Database: http://mocap.cs.cmu.edu

The exported character has a 69-joint game rig, 157,620 triangles, 10 PBR materials, 13 embedded texture images, and no morph targets. The original `CesiumMan.glb` remains in this directory as a compact importer regression fixture; see `CesiumMan-LICENSE.md` for its separate license.
