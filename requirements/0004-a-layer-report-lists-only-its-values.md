# 0004: A layer's effect lists the values it changes, apart from what it declares

## The need

A host that shows, logs or counts what a received layer will change needs the layer's
effect (`LayerReport::effect` from `check_layer_text`, and `tools::layer_effect`) to list
the values the layer sets in the data, and to keep the file's own declarations apart from
them.

Measured at Baker 0.7.0, a layer read from text that starts with
`#pack (bal (level live) (version "3") ...)` and `#origin ("ops/bal.dta" (tool t))` and sets
one value, `(economy (gold 120))`, reports three effect entries: `economy.gold`, one at the
path `bal` (the pack's name) and one at the path of the layer's own name (the origin). The
merged data holds neither path. The policy does not refuse them, although they lie outside
`allow` and are new under `existing_only`, and `promote` writes neither. So they are not
values, but they read as values to anything that walks the effect.

A host needs either:
- the effect without them, with the declarations available elsewhere (the pack and origin
  are already in `read_session()` once the layer is pushed, but not before, for a report
  made before the push); or
- each entry marked as data or declaration, so a host can tell them apart.

## Why

Every host that puts a layer's report in front of a person or a log before applying it: a
live-ops publisher listing "what this overlay changes", an editor previewing a mod, a
reviewer's diff of an agent's proposal. A count of changed values, or a list a person
approves line by line, is wrong by two for every such layer, and an entry at a path that
does not exist in the data confuses whoever reads it. Before 0.7.0, when `(pack ...)` and
`(origin ...)` were read the same way, the effect listed only the values.

## Constraints

- No extra cost for layers that declare nothing.
- The policy's verdict is unchanged: declarations stay exempt from `allow`,
  `existing_only` and `scalars_only`, as they are now.
- The pack and origin a received layer declares stay visible to a host before it pushes
  the layer: the publisher records them in a signed manifest.

## Today

This integration's tests look for the values they expect in the effect instead of
counting its entries. The publisher's output lists the two extra entries as they come.

## Not included

This note contains no code, identifiers, structure or documentation from TARGET, and
nothing confidential.
