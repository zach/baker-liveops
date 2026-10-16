# 0001: Push text a host received as a file layer, read as text only

**Status:** built in Baker at baker-dev `3ce0424`: `Config::push_layer_text`;
`check_layer_text` reads the same way (D163). The client uses it; the workaround described
under *Today* is gone.

## The need

A host that receives a layer's text from outside the game (over the network, from a
server, from a mod store) needs to push it as a layer that Baker reads exactly as it reads
a file layer, with three properties together:

1. **What the file declares is recorded.** Its `(pack ...)` (name, version, `requires`) and
   its `(origin ...)` are read with the stack's session, so `read_session()` lists the pack
   and `source()` names the origin and tool of every value it supplies, as for a file layer
   pushed by path.
2. **Text only, always.** The layer is parsed from the text the host gives, never from a
   binary cache, whatever `ConfigOptions::cache_dir` and the build say; and no cache of it is
   written. This must hold in shipping builds, which are where caches are used.
3. **No reader installed in advance.** The host hands over the text at the push. Today the
   only way to give a file layer text from memory is a `LoadSession` reader set on
   `cfg.session()` before the first layer is pushed, because every layer's session is a copy
   of the one below it.

The same push takes `LayerOptions::policy`, and the check it runs (`check_layer_text`'s
rules and the read's warnings) applies to the text as given.

## Why

Any host that applies content it did not ship: live-service games taking balance or event
overlays, games loading user mods, tools applying a proposed change from an agent or a
review service. These games are also the ones most likely to ship with `cache_dir` for
their own data, and the content they receive is exactly what D142 says must never be read
as a cache.

## Constraints

- A received layer is small (1–100 KB). Reading it should cost what `read_string` costs
  today: about 0.13 ms for 10 KB.
- Shipping builds: available, with `live_layers`.
- The layer's name in `source()` (its `file`) is chosen by the host, so it can carry an
  identifier and version, such as `received/balance@3.dta`.
- Removing the layer removes its pack and origin from `read_session()`, as `remove_layer`
  does for a file layer.

## Today

There are two workarounds, and each gives up something:

- **A reader that serves the received text by name**, installed on the Config's session
  before the base data loads, then `push_layer(name, path, {.policy = ...})`. The pack and
  origin are recorded. But a Config with `cache_dir` and edit mode off reads the layer
  through the cache path: measured at Baker 75e1728, a development build wrote a cache of the
  received layer into `cache_dir`, and a fresh one would be loaded in place of the text on
  the next push of the same name. A shipping build looks for a cache of that name before
  reading the text. So this route is only safe without `cache_dir`, and the host cannot
  check that from the Config's public interface.
- **Parse with `read_string` under a copy of `read_session()` with `text_only`, and push the
  tree.** Text only, but the tree layer has no session of its own: `source()` gives no
  origin, the pack is not in `read_session()`, and the read's warnings (an unmet `requires`)
  must be checked by the host, because `push_layer` with a tree and a policy does not see
  them.

This integration offers both and documents that the first needs a Config without
`cache_dir`.

## Not included

This note contains no code, identifiers, structure or documentation from TARGET, and
nothing confidential.
