# DrawForge experimental charter

Status: accepted project direction, pre-API

Date: 2026-08-26

Name: provisional but selected for the experiment

## 1. Thesis

DrawForge is an LLM-native semantic graphics library with a scriptable command
surface. It is not primarily a terminal paint program.

The experiment asks:

> Can a bounded, inspectable graphics document and semantic editing protocol
> help an LLM create and revise structured artwork more reliably and
> efficiently than writing SVG or graphics source directly?

The terminal application is a useful workbench: it lets a person inspect,
preview, and debug the same state an LLM or script manipulates. It is not the
authority, file format, or architectural center.

## 2. Success and kill criteria

Direct SVG authoring is the first baseline. DrawForge must demonstrate a
meaningful advantage in at least some of these dimensions:

- valid result rate;
- task completion and revision accuracy;
- unintended-change rate;
- recovery from invalid operations or stale state;
- tool calls and context tokens consumed;
- ability to continue from bounded structural inspection;
- safety and auditability of edits; and
- deterministic reproduction of the accepted result.

The project should be narrowed or stopped if a representative evaluation
corpus shows no useful advantage over direct SVG after the first semantic
toolchain prototype. Terminal novelty alone is not sufficient justification.

## 3. Product identity

DrawForge has one product center and several consumers:

1. `libdrawforge` is the terminal- and provider-neutral document engine.
2. `drawforge` is a headless command and scripting surface.
3. A future TermForge application is an interactive inspector and workbench.
4. AIForge, MCP servers, and other clients integrate through adapters outside
   the core library.

The first useful user is an LLM operating through typed tools. Human scripting
is also first-class because it makes the protocol understandable, testable,
and usable without an inference runtime.

## 4. Principles

- **The document is authoritative.** A terminal frame and an exported image are
  projections of inspectable state.
- **One mutation path.** Interactive actions, scripts, and LLM tools all submit
  the same typed operations.
- **Stable identities beat coordinate guessing.** Documents, layers, objects,
  transactions, assets, and animation tracks have durable IDs.
- **Transactions are atomic.** A rejected operation changes nothing; a batch is
  one revision and one undo step.
- **Retries are safe.** A successful transaction ID cannot be applied twice by
  a retried model or transport.
- **Results are bounded and useful.** Every transaction returns a compact
  receipt with its revision, changed identities, dirty bounds, and warnings.
- **Inspection is selective.** A client can query a subtree, selected objects,
  bounds, styles, or revision without rereading the entire document.
- **Rendering is deterministic.** Explicit inputs, time, and configuration
  produce the same evaluated scene and pixels.
- **Failures are structured.** Errors identify the operation index, field path,
  stable code, and whether retrying can help.
- **Terminal protocols stay outside the model.** Kitty, ANSI, image residency,
  and cell placement belong to TermForge.
- **Dependencies follow ownership.** DrawForge does not duplicate safe raster
  decoding that belongs to RasterForge or terminal presentation that belongs
  to TermForge.

## 5. Ownership boundaries

### DrawForge owns

- document and revision identities;
- a layer/object scene model;
- typed query and mutation operations;
- validation, transactions, receipts, and undo semantics;
- deterministic scene evaluation;
- semantic geometry operations useful to tools;
- rendering of supported scene content into bounded RGBA output;
- command encodings and script compilation;
- native persistence once its requirements are proven; and
- an evaluation harness comparing semantic tools with direct authoring.

### RasterForge owns

- decoding untrusted encoded raster media;
- validated RGBA images and views;
- bounded image fit, resize, and generic compositing operations; and
- normalization at the imported-media boundary.

DrawForge may adapt RasterForge output into its own asset and tile types. The
two public type systems remain distinct and the copy/conversion cost stays
visible.

### TermForge owns

- terminal lifecycle and input;
- widgets, layout, focus, and event delivery;
- Kitty and ANSI presentation;
- image placement, residency, accepted-write behavior, and degradation events;
- capability detection and terminal-specific performance paths.

### AIForge owns

- model-facing tool declarations and execution lifecycle;
- effects, capabilities, approval, cancellation, and provenance;
- artifact storage and prompt/context selection;
- session event history and replay; and
- the adapter that maps AIForge tool invocations onto DrawForge.

DrawForge must not expose AIForge, TermForge, provider, JSON-library, or
terminal-protocol types through its core public API.

## 6. Dependency direction

```text
                     scripts / JSON transactions
                               |
  TermForge workbench ---- drawforge CLI ---- external adapters
              \                |                /
               \               |               /
                +--------- libdrawforge -------+
                           |         |
                       geometry    rendering
                           |         |
                       document + commands

  AIForge adapter lives in AIForge and depends on a released DrawForge API.
```

The core can be loaded, queried, edited, evaluated, and rendered without a live
terminal, an LLM backend, or a filesystem. Ports may be introduced for asset
storage and persistence when the requirements demand them.

## 7. Three interface layers

The project must not confuse its domain API, interchange protocol, and human
scripting experience.

### 7.1 Typed C++ domain API

The authoritative API uses explicit value types, invariants, and
`std::expected`. JSON is an adapter concern. A transaction is typed data, not a
callback and not executable source.

### 7.2 Stable command encoding

A versioned JSON encoding carries bounded queries and transactions across CLI,
pipe, and tool boundaries. Newline-delimited JSON is the initial streaming
transport, not necessarily the persisted document format.

Illustrative transaction shape, not an accepted schema:

```json
{
  "document": "scene",
  "expected_revision": 4,
  "transaction_id": "add-sun-v1",
  "operations": [
    {
      "op": "shape.create",
      "id": "sun",
      "kind": "ellipse",
      "bounds": {"x": 480, "y": 40, "w": 96, "h": 96},
      "style": {"fill": "#ffd54a"}
    }
  ]
}
```

An accepted receipt should report the resulting revision, created and changed
IDs, dirty bounds, warnings, and transaction identity. Resubmitting the same
successful transaction ID returns the established receipt rather than applying
the change again.

### 7.3 Scene scripting

A decent scripting experience compiles into the same typed transactions. JSONL
alone is a good transport but is not presumed to be the final authoring syntax.
Language selection is deferred until real scripts reveal required constructs.

Likely ergonomic needs include:

- readable caller-selected IDs;
- references to values created earlier in one transaction;
- named styles, palettes, and reusable symbols;
- group-local coordinates;
- semantic alignment, distribution, and transform operations;
- parameters and deterministic repetition;
- dry-run and explain modes; and
- source-located diagnostics.

Arbitrary document-supplied code, ambient filesystem access, networking, FFI,
and process execution are out of scope for the first scripting milestone.

## 8. LLM tool shape

Avoid a separate model-facing tool for every primitive property. A small suite
keeps schemas and tool selection tractable while a bounded transaction retains
semantic operations.

The initial integration target is approximately:

- `drawforge_create` — create a bounded in-memory document;
- `drawforge_inspect` — query revision, structure, objects, styles, and bounds;
- `drawforge_apply` — dry-run or commit one atomic transaction;
- `drawforge_render` — evaluate and render a preview artifact; and
- `drawforge_export` — explicitly persist an approved output.

Exact names and schemas require prototype evidence. The DrawForge library
provides domain operations and schema material; an AIForge adapter owns
`ToolExecutor`, effects, capabilities, events, and artifact references.

Editing an in-memory document is a write effect scoped to that document.
Inspection and rendering are reads. Importing an artifact reads that artifact;
exporting to a path is a filesystem write. The adapter must not route ordinary
drawing through a shell or grant ambient filesystem access.

## 9. Initial document slice

The first semantic prototype needs only enough model to evaluate the thesis:

- canvas extent and background;
- ordered layers and groups;
- rectangle, ellipse, and path objects;
- fill, stroke, opacity, and affine transform;
- stable caller-selected object IDs;
- selection by ID and bounded structural query;
- revisions, atomic transactions, retry receipts, and undo;
- one animatable property, initially position or opacity; and
- deterministic RGBA and PNG preview output.

Text, raster brushes, filters, masks, tile storage, SVG import, native archives,
timeline UI, and general animation remain deferred. Imported raster assets may
be added when they are needed for the evaluation corpus.

## 10. Evaluation loop

Every representative task should run through at least two routes:

1. direct SVG or source authoring;
2. DrawForge semantic inspection and transactions.

Candidate tasks include:

- create a small icon from a description;
- revise one named element without disturbing the rest;
- recolor a theme while preserving geometry;
- align and distribute several objects;
- group and transform a component;
- add a simple animated entrance;
- recover from an invalid operation;
- recover from an expected-revision conflict; and
- continue after context compaction using bounded scene inspection.

The harness records model and prompt identity, accepted operations, retries,
errors, tool calls, token usage when available, final semantic invariants, and
rendered artifacts. Pixel goldens test engine determinism; they are not by
themselves a quality judgment for stochastic model output.

## 11. Roadmap phases

### Phase 0: experimental contract

- define the evaluation corpus and direct-SVG baseline;
- prototype the smallest useful scene and query model;
- decide the renderer boundary through a measured spike;
- specify operation, transaction, receipt, and error invariants; and
- publish a versioned example schema without promising compatibility.

### Phase 1: semantic core and headless loop

- implement document identities, objects, layers, and queries;
- implement atomic retry-safe transactions and undo;
- render deterministic RGBA/PNG previews;
- expose inspect, dry-run, apply, and render through the CLI; and
- run the initial comparison corpus.

The project passes its first gate only if this loop provides evidence that the
semantic route is useful.

### Phase 2: scripting experience

- collect real transaction scripts from the corpus;
- choose the smallest authoring representation that improves on raw JSONL;
- compile scripts into the same domain transactions;
- add parameters, reusable definitions, and source diagnostics only as proven
  necessary; and
- preserve deterministic, bounded execution.

### Phase 3: workbench

- build a TermForge inspector and preview canvas;
- show document tree, selection, transaction receipts, errors, and revisions;
- add minimal direct manipulation as a debugging aid; and
- verify the workbench and headless paths evaluate the same scene.

### Phase 4: AIForge adapter

- consume a released DrawForge API from AIForge;
- register the bounded tool suite in AIForge's runtime registry;
- map document and artifact access to effects and capabilities;
- store previews through AIForge's artifact boundary;
- record receipts without copying whole documents into run events; and
- add deterministic fake-driven integration scenarios before live inference.

## 12. Explicitly deferred decisions

The roadmap must resolve these with prototypes rather than assumption:

- renderer and geometry dependency strategy;
- path representation and deterministic rasterization contract;
- command-schema evolution and compatibility policy;
- scene-script representation;
- document persistence and archive format;
- PNG or other encoded-output ownership;
- raster storage and undo representation;
- visual diff and preview summarization; and
- the model/corpus mix used for comparative evaluation.

The project does not begin by selecting a plugin ABI, embedded general-purpose
runtime, collaborative editing model, color-management system, video encoder,
or terminal-specific document concept.
