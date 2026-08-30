Add a simple non-looping opacity entrance to `dot`. Keep its geometry and
style, including initial opacity 0. Add an opacity track named `dot-entrance`
from 0 to 1 over 600 milliseconds, starting at time zero and holding its final
value. Preserve `canvas` exactly.

Use the DrawForge semantic route. Inspect the supplied document when present
and submit newline-delimited `drawforge.experimental/v1` request frames. Apply
all changes through bounded atomic transactions, preserve caller-selected IDs,
and use the current revision. Do not submit SVG or invent another document
encoding.
