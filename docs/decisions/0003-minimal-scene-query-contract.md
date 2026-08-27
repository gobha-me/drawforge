# ADR-0003: Minimal scene and bounded-query contract

Status: accepted for the Phase 0 contract

Date: 2026-08-27

Issue: [#5](https://github.com/gobha-me/drawforge/issues/5)

## Context

The first semantic evaluation needs enough document structure to create and
revise the nine checked-in corpus scenes without making SVG, JSON, a renderer,
or a terminal model authoritative. It also needs selective inspection after a
context reset; returning an unbounded document would erase the intended
advantage over complete source replacement.

This contract describes the domain values that issue #8 will implement. It
does not add a persistence format, interchange encoding, mutation API, or
renderer dependency. Issue #6 owns transaction and receipt semantics, issue
#7 owns JSON encoding, and issue #10 owns the production rendering adapter.

## Decision

### Document and identity model

A document has a `DocumentId`, a `Revision`, immutable `ResourceLimits`, a
`CanvasExtent`, an optional canvas background, and an ordered sequence of
layers. No background means transparent black for rendering; it does not add a
scene node.

Each layer has a `LayerId`, a visibility flag, and an ordered sequence of
objects. A document may be empty and a layer may have no children. Layers are
root-only containers: they cannot be nested or parented by objects.

Groups and drawable objects share one document-wide `ObjectId` namespace.
Every object has exactly one parent, either a layer or a group. A group owns an
ordered sequence of child objects. Reparenting preserves no implicit position:
the object's local transform is interpreted in its new parent's coordinate
space. Parent cycles and multiple parents are invalid.

The `max_scene_nodes` budget counts layers, groups, and drawable objects
together. The document root and animation tracks do not count as nodes. At
most one opacity track may target a drawable, so track cardinality is already
bounded by the drawable count. Nesting depth counts object edges below a layer:
a direct layer child has depth one and must not exceed `max_nesting_depth`.

### Common object state and ordering

Every group and drawable has a visibility flag and a local
`AffineTransform`. Groups have no fill, stroke, or opacity of their own in the
initial slice, avoiding style inheritance and isolated-group compositing.
Drawables additionally have a style and an opacity.

Canvas background is painted first, then visible layers in layer order. Within
a layer or group, visible children are visited in child order; a group visits
its descendants depth-first at its position in that order. Later paint covers
earlier paint. A node is effectively visible only when its own flag, its layer,
and every ancestor group are visible.

For a point in object-local coordinates, transforms apply from the object
outward through its ancestor groups. Using ADR-0002's composition operation,
the document transform is `local.then(parent).then(grandparent)...`. Layer and
canvas containers have no transform in this slice.

### Color, style, and geometry

Colors are four eight-bit straight-alpha sRGB channels. There is no color
profile, palette reference, named color, or premultiplied storage in the
domain. Opacity is a finite `double` from zero through one. A drawable's
effective source alpha is its color alpha multiplied by its evaluated opacity.

A style contains an optional fill and an optional stroke. A stroke contains a
color and a finite width greater than zero. Line caps are fixed to butt, line
joins are fixed to miter, and the miter limit is fixed to four. A drawable with
neither fill nor stroke is valid but paints nothing. Styles do not inherit
through groups.

The accepted drawable kinds are:

- **Rectangle:** origin, non-negative width and height, and non-negative x/y
  corner radii. Each radius must be no greater than half its corresponding
  extent; invalid radii are rejected rather than clamped.
- **Ellipse:** center and non-negative x/y radii. A circle uses equal radii.
- **Path:** a non-empty sequence of `move_to`, `line_to`, and `close` commands.
  The first command and the first command after a closed subpath must be
  `move_to`; `line_to` and `close` require an open subpath; empty subpaths and
  repeated `close` commands are invalid. Fill implicitly closes every open
  subpath, while stroke follows only explicit segments and a `close` segment.

Zero-width rectangles, zero-height rectangles, and zero-radius ellipses are
valid empty geometry. Negative, non-finite, out-of-range, or arithmetically
unrepresentable geometry is invalid. Curves, arcs, fill rules, configurable
caps/joins, clipping, masks, and boolean geometry are deferred.

### Opacity animation

Opacity is the sole animatable property because it is the only animation
required by the v1 corpus. An opacity track has a `TrackId`, targets one
drawable, and stores a non-negative start time, a positive duration, and
validated from/to opacity values. Times are integer microseconds. A drawable
may have at most one opacity track; tracks do not appear as scene children.

Evaluation requires an explicit non-negative time. Before a track starts, the
drawable uses its authored opacity. From the start through the end time,
opacity is linearly interpolated, with the exact `from` value at the start and
the exact `to` value at the end. After the end it remains at `to`. Repetition,
easing, alternate directions, additive animation, and animation of groups are
deferred.

### Bounds

Bounds are axis-aligned in the requested coordinate space and use validated
`Coordinate` values. Empty bounds are represented explicitly as no bounds,
not as a zero rectangle at an arbitrary origin. Bounds arithmetic is checked;
overflow or a value beyond the document's numeric limit fails the complete
query.

Three bounds projections are accepted:

1. **Local geometry bounds** contain the drawable geometry before its local
   transform. For a group, they are the union of descendant geometry expressed
   in that group's coordinate space.
2. **Document geometry bounds** apply the complete ancestor transform chain but
   ignore visibility, fill, stroke, and opacity.
3. **Document painted bounds** contain the transformed fill and exact stroked
   outline at the query's evaluation time. They are empty for effectively
   invisible objects, unpainted objects, and objects whose evaluated opacity is
   zero.

Rectangle and line-path bounds are derived from their edges; ellipse bounds
use the exact affine ellipse extrema. Group and layer bounds are the union of
their descendants for the selected projection. These semantic bounds remain
independent of renderer pixel extents and dirty-region rounding.

### Bounded queries

Queries are typed, read-only operations. Every successful response owns its
values and includes the `DocumentId` and observed `Revision`; it never exposes
a borrowed view into mutable document storage. The accepted query families
are:

- **Document summary:** revision, canvas, background, limits, and layer/object/
  track counts.
- **Structure:** a document, layer, or group root plus explicit maximum depth
  and maximum returned nodes; results contain identity, kind, parent,
  sibling index, visibility, child count, and children in document order.
- **Selected objects:** an explicit ordered list of object IDs and an explicit
  field set chosen from kind, parent/order, visibility, transform, geometry,
  style, and opacity-track metadata.
- **Bounds:** an explicit ordered list of layer or object IDs, one or more of
  the three bounds projections, and an explicit non-negative evaluation time.

For a layer or group root, the root descriptor counts against `max_nodes`; the
document root is response metadata and does not. A zero `max_depth` returns
only the selected layer/group root, or no scene nodes for a document root.
Depth is relative to the selected root. Its maximum is
`max_nesting_depth + 1` for a document root, to account for the layer edge, and
`max_nesting_depth` for a layer or group root. `max_nodes` must be positive and
no greater than `max_scene_nodes`. A document configured with a zero scene-node
budget is inspected through the document-summary query instead.

Selection cardinality must not exceed `max_scene_nodes`. Duplicate selectors
are invalid. Selected-object and bounds results follow request order;
structure results follow document order.

A missing ID, an inapplicable property, failed bounds computation, or a result
that would exceed an explicit query budget fails the whole query without a
partial response. Clients obtain a smaller view by selecting a narrower root,
shallower depth, smaller projection, or explicit IDs. The first contract does
not add cursors or continuation tokens; the evaluation corpus contains at most
nine nodes, while the explicit failure preserves bounded behavior for larger
documents.

The semantic failure categories are `missing_identity`, `duplicate_identity`,
`invalid_parent`, `parent_cycle`, `invalid_geometry`,
`unsupported_node_kind`, `unsupported_property`, `resource_limit`,
`number_out_of_range`, and `arithmetic_overflow`. Foundation construction
retains the `ValueErrorCode` categories from ADR-0002. Issue #6 will attach
operation index, field path, and retryability to transaction failures; issue
#7 will define how these names are encoded and how unknown wire values fail.

## Corpus-shaped examples

These examples describe semantic state, not a JSON or persistence encoding.

### Status badge

```text
document status-badge, canvas 160 x 64, transparent background
  layer artwork
    rectangle badge: (0, 0), 160 x 64, radii 16 x 16, fill #172033ff
    ellipse indicator: center (32, 32), radii 12 x 12, fill #35c46aff
    path check: move (26, 32), line (30, 36), line (38, 27),
                no fill, stroke #ffffffff width 3
```

This covers canvas state, layer and child ordering, every accepted drawable
kind, rounded geometry, stroked line paths, stable IDs, and transparent
background. The filled `hills` path in the sun-revision fixture uses the same
line-path model with an explicit final `close` and a fill-only style.

### Grouped mascot and selective inspection

```text
document mascot, revision 4, canvas 192 x 144, background #ffffffff
  layer artwork
    rectangle frame
    group mascot: transform translate(24, 12)
      ellipse body
      ellipse eye-left
      ellipse eye-right
```

The world transform for each mascot child applies its own local transform and
then the group's translation. A structure query rooted at `mascot` with depth
one and four-node budget returns the group followed by its three children. A
selected-object query for `eye-right`, then `body`, with only parent/order and
geometry fields returns exactly that order and no style fields. Hiding the
group makes every descendant's painted bounds empty without changing geometry
bounds or authored child properties.

### Opacity entrance

```text
drawable dot: authored opacity 0
track dot-entrance: target dot, start 0 us, duration 600000 us,
                    from 0, to 1, linear, freeze
```

The evaluated opacity is exactly 0 at 0 us, 0.5 at 300000 us, and 1 at and
after 600000 us. The track has no scene-tree parent and does not alter the
dot's geometry or style.

### Corpus coverage

| Corpus behavior | Accepted semantic surface |
| --- | --- |
| create the status badge | canvas, ordered layer, rounded rectangle, ellipse, stroked path |
| revise the named sun and recolor the card | selected-object geometry/style projections |
| align toolbar icons | explicit geometry fields and request-ordered selection |
| group and move the mascot | reparenting, child order, group-local transform |
| animate the dot entrance | one frozen linear opacity track at explicit time |
| recover from invalid input | stable semantic failure category and no partial query result |
| recover from a stale revision | observed revision on every query response |
| continue after compaction | bounded structure, selected fields, and bounds queries |

The semantic fixtures are authored directly against this model. SVG fixture
details that have no domain equivalent are not imported: for example, a fill
written once on an SVG group is represented as the same explicit fill on each
semantic child. This preserves the evaluated scene without adding SVG import
or style inheritance to the first gate.

### Failure-first conformance matrix

| Candidate | Required result |
| --- | --- |
| duplicate layer, object, or track ID in its namespace | `duplicate_identity` |
| missing parent, query root, target, or selected ID | `missing_identity` |
| drawable used as a parent or layer nested below an object | `invalid_parent` |
| direct or indirect group cycle | `parent_cycle` |
| negative radius, excessive corner radius, or malformed path state | `invalid_geometry` |
| curve node or text node | `unsupported_node_kind` |
| request for an unaccepted property or second opacity track on one drawable | `unsupported_property` |
| scene/depth/query limit exceeded | `resource_limit` with no partial state or response |
| transformed bounds exceed numeric magnitude | `number_out_of_range` |
| intermediate bounds arithmetic is unrepresentable | `arithmetic_overflow` |
| same scene, revision, query, and time repeated | value-for-value identical semantic result |

Happy-path examples are evaluated only after this failure matrix. Issue #8
must turn the matrix into executable tests before implementing broader scene
behavior.

## Consequences

- Issue #6 can define typed operations against one unambiguous hierarchy and
  can use document-space painted bounds in receipts without selecting a
  renderer.
- Issue #7 can encode a small fixed set of node, property, query, and error
  kinds while keeping JSON out of the in-memory model.
- Issue #8 implements this contract behind `std::expected` public boundaries;
  this ADR alone does not add a compiled scene API.
- Issue #10 evaluates the accepted scene at explicit time but cannot redefine
  semantic bounds from renderer pixel extents.
- SVG import/export, persistence, curves, text, raster content, style
  inheritance, broad animation, and terminal or provider concepts remain out
  of scope for the first evaluation gate.
