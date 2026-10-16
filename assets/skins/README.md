# Drop-in skins

Put a character model here (`MyHero.fbx` or a baked `MyHero.skm`) and restart: it becomes the skin
`myhero` (Pause > Skin, `cl_skin myhero`), seen by every player. No code, no JSON needed.

Optional `MyHero.json` next to it: `name`, `atlas` + `tiles` (texture atlas, like
`assets/human/skins.json`), `aliases` (bone renames, see `tools/bake_mesh --write-sidecar`) and
`sockets`. The rig must be compatible with the human Skeleton (docs/CHARACTERS.md); an incompatible
one is reported in the log with suggested aliases.
