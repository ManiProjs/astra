# Terrain material

`forest_ground_04/` contains the PBR material **Forest Ground 04** from [Poly Haven](https://polyhaven.com/a/forest_ground_04), released under **CC0 1.0 Universal**.

- `forest_ground_04_diff_1k.jpg`: runtime sRGB base colour, downsampled from the retained 2K source.
- `forest_ground_04_nor_gl_1k.png`: runtime tangent-space OpenGL normal map, downsampled from the retained 2K source.
- `forest_ground_04_arm_1k.png`: runtime packed ambient occlusion (R), roughness (G), metallic (B); Astra binds this image to both the metallic-roughness and occlusion slots.
- The original `*_2k.*` maps remain available for higher-memory targets.

Artist credits: Rob Tuytel (photography/processing), Rico Cilliers (minor adjustment).

CC0: https://creativecommons.org/publicdomain/zero/1.0/
