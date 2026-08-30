Create a 160 by 64 status badge. Add a background rectangle named `badge` at
(0,0), size 160 by 64, corner radius 16, and fill `#172033`. Add a circle named
`indicator`, center (32,32), radius 12, and fill `#35c46a`. Add a path named
`check` with path data `M26 32l4 4 8-9`, no fill, white (`#ffffff`) stroke, and
stroke width 3.

Use the DrawForge semantic route. Inspect the supplied document when present
and submit newline-delimited `drawforge.experimental/v1` request frames. Apply
all changes through bounded atomic transactions, preserve caller-selected IDs,
and use the current revision. Do not submit SVG or invent another document
encoding.
