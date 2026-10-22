// Night vision: the near-IR image (docs/NIGHT_OPTICS.md "Near-IR layer"). Filled in by the near-IR
// pass; until it runs the intensifier sees the visible image only.
#include "post_internal.h"

FgRes post_nvg_nir(void) { return FG_NONE; }
