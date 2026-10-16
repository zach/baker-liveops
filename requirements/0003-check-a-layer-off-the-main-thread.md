# 0003: Check a candidate layer off the thread that owns the Config

**Status:** built in Baker at baker-dev `3ce0424`: `Config::checker()` and
`ValidatorOptions::any_thread` (D166, D167). The client uses it; the workaround described
under *Today* is gone.

## The need

A host needs to check a candidate layer (`check_layer`, `check_layer_text`) on a worker
thread while the thread that owns the Config keeps reading it, and may change it. The
check would run against a fixed state of the stack, for example a `ConfigSnapshot` or the
root and session as of an `epoch()`, and report against that state, so the owning thread
can tell later whether the report still describes its Config.

## Why

The check runs when a layer arrives, which is any frame. A game, a dedicated server, or an
editor receiving content wants the work off its frame: parsing, merging the candidate over
the stack, and running the validators are independent of what the frame is doing. The
owning thread then applies it at a boundary (see 0002).

## Constraints

- Measured at Baker 75e1728, shipping build: 1.2–1.5 ms for a 10 KB layer over a
  2,000-entry table, about 1.8 ms with `shape::validator`. That is the hitch a game takes
  today on the frame the layer arrives.
- The worker thread must not change anything the owning thread reads, and must not need a
  lock the owning thread takes per frame.
- Validators run on the worker thread, so a host must be able to tell which validators are
  safe to run there, or Baker documents that they must be.
- Allocations come from an allocator the host chooses for the worker.

## Today

The check runs on the thread that owns the Config, when the layer arrives, because the
Config is not thread-safe. Its cost lands on that frame.

## Not included

This note contains no code, identifiers, structure or documentation from TARGET, and
nothing confidential.
