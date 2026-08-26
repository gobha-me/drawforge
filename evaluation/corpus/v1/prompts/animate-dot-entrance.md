Add a simple non-looping opacity entrance to `dot`. Keep its geometry and
style, including initial opacity 0. Add a child `animate` element with ID
`dot-entrance`, attributeName `opacity`, from 0, to 1, duration `600ms`, and
fill `freeze`. Preserve `canvas` exactly. Return the complete SVG.
