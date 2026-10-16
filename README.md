# Baker for live ops

An integration of [Baker](https://github.com/zach/baker) with the places a live game keeps
its overlays. It publishes, delivers, and retires *published overlays*: versioned, checked
layers that change a shipped game's data without a client patch, applied at a boundary
the game chooses, rolled back in one call, and promoted into source once they stick.

- **`publish`** (command line).
  - Check an overlay against the base data it targets, with the game's own policy. Its
    `requires` and versions are checked against the base's packs, and its shapes are
    validated.
  - Run the stack's scenarios with it, and refuse it if an objective breaks.
  - Write a manifest (name, version, requires, content hash, signature, policy) and
    upload both through a backend.
- **`client`** (a C++ helper a game links).
  - Fetch the manifest and overlay through a transport the host supplies, verify the hash
    and signature before Baker sees a byte, and stage the overlay with
    `Config::check_layer_text`.
  - Apply it at the host's boundary; keep the last good overlay for an offline start; roll
    back.
- **Backends:**
  - `static/`: any HTTPS prefix, CDN or bucket.
  - `nakama/`: Nakama storage objects per environment.
- **`retire`:** a pull request that promotes a stuck overlay into the base data.

The convention it implements is Baker's cookbook, "Published overlays". The plan it follows
is plan D of baker-dev's `dev/integration-plans.md`.

## License

This repository is licensed under the MIT License (`LICENSE`), as Baker is. Baker is used
here as a dependency; nothing in this repository is part of Baker. Nakama is a product of
its owners, who have not endorsed this project.

## Building

Baker is fetched at a released version:

```cmake
include(FetchContent)
FetchContent_Declare(baker GIT_REPOSITORY https://github.com/zach/baker.git GIT_TAG vX.Y.Z)
FetchContent_MakeAvailable(baker)
```

## Requirements for Baker

When this integration needs something Baker lacks, write it down in `requirements/` from
`requirements/0000-template.md`, in Baker's own terms. Only the text of a requirement goes
to Baker.
