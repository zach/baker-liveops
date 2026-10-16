# Working on this integration

This repository integrates Baker with live-ops storage and backends. Baker is MIT-licensed and must stay free of
live-ops storage and backends's terms; baker-dev's `INTEGRATIONS.md` is the policy. For work here:

- **Nothing from this repository goes into Baker.** Do not copy code, identifiers, comments,
  structure or documentation from here, or from live-ops storage and backends, into baker-dev or zach/baker, and do
  not write Baker code from memory of live-ops storage and backends's.
- **A need goes to Baker as a requirement note,** in `requirements/`, from
  `requirements/0000-template.md`, written in Baker's terms: what a host needs, why as a
  general use case, and the constraints. The owner takes the note's text to Baker.
- **Confidential material stays here.** What live-ops storage and backends's SDK or agreement discloses does not go
  into a requirement note, a Baker issue, a pull request, or a Baker session.
- **Do not start Baker work from this session.** Changes to Baker happen in a separate
  session in baker-dev, from the note alone.
- **Use Baker as a user would:** a released version, through its public headers. A
  workaround for a Baker limit lives here until Baker ships the feature.

## This integration

- **Baker decides; this repository delivers.**
  - Checking an overlay is Baker's `check_layer_text` under a `LayerPolicy`, and applying
    it is `push_layer` with the same policy. Do not re-implement either here.
  - If delivery needs something Baker lacks, write a requirement.
- **Trust.**
  - An overlay from storage is untrusted until its content hash and Ed25519 signature
    verify.
  - Baker then reads it as text only, never a cache.
  - Keys never enter the repository, its tests, or its CI logs.
- **No vendor SDK in the core.** `client/` and `publish/` talk to storage only through a
  backend interface. A commercial store (a platform's title storage, a flag service) is a
  backend in a separate, private repository.
- **The game chooses when.** The client stages as soon as an overlay arrives, and applies
  only when the host says so: a session's start, a lobby. Dedicated servers use the same
  client and manifest.
- **Tests run without the network,** against a directory standing in for storage, and a
  Nakama container in CI.
