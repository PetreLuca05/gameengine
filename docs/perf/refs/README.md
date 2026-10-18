# Reference captures

One reference shot per benchmark scene (camera key 1 of 3, Modern, UI hidden), downscaled to 800 x 450,
from the first run (2026-10-18, Mesa lavapipe: see docs/PERF_AUDIT.md "First run"). They show what each
scene renders, so a broken scene setup is visible at a glance.

The comparisons of `perf_verify` do not use these files: every A / B run writes its own full-resolution
shots on a fixed TAA phase next to its profiles (`<build>/perf_out/verify/<fix>/`, the CI artifacts
`perf-audit-<run>`), and compares B with A of the same run on the same machine, because images are
only comparable on one GPU and driver.
