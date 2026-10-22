# Local BMX Streets reference extraction

This directory holds locally extracted references from the user's installed copy of BMX
Streets. Generated contents are ignored by Git. Source assets retain their original ownership;
the local extraction does not establish redistribution permission.

The extraction script one directory above reads selected UnityFS bundles without running or
modifying the game. `manifest.json` records exact source paths, SHA-256, object identities and
export errors. Wavefront meshes preserve original part geometry; adjacent transform/material
JSON records preserve the source assembly information. These files are not yet a Frostbite
content pack and do not by themselves demonstrate in-game rendering or rider animation.
