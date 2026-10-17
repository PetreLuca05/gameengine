# Unity-style shadow controls: captures

Headless captures (Xvfb + lavapipe, 1280x720, dev room, Modern tier, 0 validation errors).
See docs/SHADOWS_UNITY_MODEL.md for the knobs.

| Files | What |
|---|---|
| u01..u05 | Sun at the shadow corner, clear noon: Soft Ultra (PCSS), Soft High / Medium / Low (tent 7 / 5 / 3), Hard (`sun_shadow type hard`) |
| u06 | `sun_shadow strength 0.5`: half the darkening |
| u07 | `sun_shadow type none` |
| u08 | `r_shadows 1` (Hard Only): the Soft sun renders hard |
| u09 | Zoomed shadow edges, left to right: Ultra, tent 7, tent 5, tent 3, Hard |
| u10..u14 | `r_shadow_debug 4` cascade colours: 4 cascades (Stable Fit), readout + split bar (`r_shadow_debug 1`), 2 cascades, 1 cascade, 4 cascades Close Fit |
| u20..u25 | Night, hall lamps: level settings, `light_shadow all type hard`, `... type soft` + tent 7, `... strength 0.5`, readout |
| u30 | Settings > Graphics (the Shadows group is further down the page) |
| u40..u46 | Night, the dev room demo pair at x = -22: blue lamp Hard (crisp player shadow), orange lamp Soft (blurred edge), the orange lamp switched to hard / soft tent 7 / none with `light_shadow 45 ...` |
| u47 | Overview grid of u40..u46 |
