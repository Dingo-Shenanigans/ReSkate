# FPV drone models

Both are licensed CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/), which allows them to
be shipped, changed and shared as long as the author is credited and changes are noted.

| File | Title | Author | Source |
| --- | --- | --- | --- |
| `fpv_drone.glb` | FPV Drone | PinaPipe (https://sketchfab.com/PinaPipe) | https://sketchfab.com/3d-models/fpv-drone-92152c31ec9a4a87b2a0478bd114d293 |
| `dji_avata2.glb` | DJI Avata2 | raphael.harris.gaffga (https://sketchfab.com/raphael.harris.gaffga) | https://sketchfab.com/3d-models/dji-avata2-e27ed758e2174a89a48368e84027f8d9 |

`drone.glb` is the game model, made from `fpv_drone.glb` by `tools/models/prepare_drone.py`:
reduced from 27,946 to about 8,000 triangles, textures shrunk to 512 pixels, scaled to 0.29 m
across, centred and turned to face forward:

    blender --background --python tools/models/prepare_drone.py -- assets/models/fpv/fpv_drone.glb assets/models/fpv/drone.glb --triangles 8000 --span 0.33 --forward 0.305,-0.952 --texture-size 512

In game the credit reads: "Drone model: FPV Drone by PinaPipe (CC BY 4.0)".
