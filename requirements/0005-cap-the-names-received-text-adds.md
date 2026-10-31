# 0005: Cap the names a received layer may add, apart from the game's own data

## The need

A host that pushes text it received (`check_layer_text`, `push_layer_text`, a
`LayerChecker`'s `check_text`) needs to cap the new names that text may add to the program,
at a limit of its own choosing, without lowering the limit for the game's own data.

Measured at Baker 0.7.0:
- `ParseLimits::max_new_symbols` set on `cfg.session().limits` before anything is pushed
  caps a file or text layer of the game's own data, but `check_layer_text` and
  `push_layer_text` of 300 new names both succeed under a cap of 100.
- Set after the base is pushed, it caps neither.
- `read_string` with the cap on a copy of `read_session()` stops at the 101st new name, as
  documented.

So the documented advice, "set it far lower for text you do not trust", cannot reach the
reads that take text a host does not trust.

## Why

Overlays applied all session, mods, and anything a console or a server sends: symbols live
until the program ends, so each such read can keep names for good. The game's own data
legitimately needs the default (1,048,576), and a received layer needs perhaps a few
thousand, so one limit for both is either useless for the received text or breaks the
game's data.

## Constraints

- Set once, for every received layer, for example on `LayerOptions`, on `LayerPolicy`, or
  as a Config option for received text, and honoured by `LayerChecker` checks too, which run
  off the Config's thread.
- A read that passes the cap costs nothing extra. A read that fails it is refused, located
  as it is today (`ErrorCode::Limit`), and changes nothing, under a policy or without one.
- The names a refused read already added are bounded by the cap.

## Today

This integration reads each verified overlay once with `read_string`, under the cap on a
copy of `read_session()` (text only, includes refused), before it checks it. The check then
finds the names already interned. The cost is one extra parse of the overlay, about 0.1 ms
for 10 KB, off the Config's thread.

## Not included

This note contains no code, identifiers, structure or documentation from TARGET, and
nothing confidential.
