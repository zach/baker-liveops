# 0002: Apply a layer that was checked, without checking it again

**Status:** built in Baker at baker-dev `3ce0424`: `LayerOptions::checked` (D165). The
client uses it; the workaround described under *Today* is gone.

## The need

A host checks a layer when it arrives (`check_layer_text` under a `LayerPolicy`), and
applies it later at a moment it chooses, under the same policy, so that a layer whose
verdict changed in between is still refused. When nothing in the Config changed between
the check and the push, the push should cost what an unchecked push costs: the read (if any),
the merge, the rebinding and the callbacks, and not a second check.

The host needs to be able to say "push this, under this policy, given this report", and
Baker to decide whether the report still holds (the same stack, the same validators, the
same text) or to check again if it does not. A host should not have to reason about
`epoch()` itself, because some changes that can alter a verdict do not change the epoch
(adding or removing a validator; a fast-path `set()` in a development build).

## Why

Every host that separates "it arrived" from "it takes effect":
- live games that apply overlays at a session start or in a lobby, never mid-match;
- dedicated servers that apply between rounds;
- editors and tools that show a change's report, then apply it when a person accepts.

The second check is the largest cost of applying, and it is paid at the boundary, where a
game has the least time.

## Constraints

Measured at Baker 75e1728, Release, GCC 13, 4 vCPU Linux container, shipping build. A
10 KB overlay changes 256 values of a 2,000-entry keyed table, under a policy with every
rule on and `allow` set:

| | plain | with `shape::validator` |
|---|---|---|
| `check_layer` (tree) / `check_layer_text` | 1.44 / 1.65 ms | 1.73 / 1.95 ms |
| `push_layer` (tree), no policy | 0.14 ms | 0.45 ms |
| `push_layer` (tree), with the policy | 1.56 ms | 2.16 ms |

The goal for applying a 10 KB overlay at a boundary is under 1 ms of main-thread time; the
push without its second check meets it, the push with it does not.

- No allocation beyond what the push makes today.
- Shipping builds, with `live_layers`.
- A report that no longer holds must never be used: then the push checks, and refuses as
  it does today, changing nothing.

## Today

The host remembers `epoch()` when it checks and, if it is unchanged at the boundary, pushes
without the policy; otherwise it pushes with it. This is opt-in here, off by default,
because it relies on the host's reading of what can change a verdict.

## Not included

This note contains no code, identifiers, structure or documentation from TARGET, and
nothing confidential.
