# Baker for live ops

An integration of [Baker](https://github.com/zach/baker) with the places a live game keeps
its overlays. It publishes, delivers, and retires *published overlays*: versioned, checked
layers that change a shipped game's data without a client patch, applied at a boundary
the game chooses, rolled back in one call, and promoted into source once they stick.

It follows plan D of baker-dev's `dev/integration-plans.md` (the refined plan), on Baker's
layer check (`Config::check_layer_text`, `LayerPolicy`, `LayerOptions::policy`, D161).

| Directory | What |
|---|---|
| `client/` | The C++20 helper a game links: verify, stage, apply at the host's boundary, keep the last good, roll back, report what is live. libsodium; no vendor SDK. |
| `publish/` | `baker-liveops`, the command line: `check`, `publish`, `withdraw`, `show`, `keygen`, and `retire` (`retire/`). |
| `backends/static/` | Files under a prefix: any HTTPS host, CDN or bucket. A directory stands in for it in the tests. |
| `backends/nakama/` | Nakama storage objects per environment, served by runtime RPCs (a Lua module), and the publisher's command for it. |
| `tests/` | The client's tests, the end-to-end tests, the timings, a test game, and Ledger's description. |
| `requirements/` | What this integration needs from Baker, in Baker's terms. |

## The convention

**An overlay is a pack.** Its first form names it, its version, and the base it was made for;
its second says where it came from, so `source()` names it:

```dta
(pack balance_2026_10 (level live) (version "3") (requires (game_data "1.4")))
(origin "ops/balance_2026_10.dta" (tool baker-liveops))
(economy
   (gold_per_win 120))
```

**The game declares its policies in data** that ships with it (`liveops.dta`), so the
publisher checks an overlay exactly as the game will:

```dta
(liveops
   (pack_levels game live)
   (include_dirs data)
   (layers (default data/default.dta))
   (shapes 1)
   (policies
      (hotfix (data_only 1) (scalars_only 1) (existing_only 1) (warnings_refuse 1)
              (allow economy events))))
```

**The manifest** lists an environment's overlays, bottom to top, each with its version,
`requires`, SHA-256, size, and policy name. It is signed as a whole with Ed25519 and carries a
sequence number that rises with every publish, so an old manifest served again is refused
as a replay (`client/include/baker_liveops/manifest.hpp`).

## The client

```cpp
baker_liveops::Client live(cfg, {.environment = "prod", .keys = trusted, .policies = policies,
                                 .transport = &cdn, .store = &user_storage});
live.attach(cfg.session());           // before the base loads
cfg.push_layer("default", "data/default.dta");
cfg.freeze();                          // ConfigOptions::live_layers = true
live.restore();                        // the last good overlays, verified and checked again
live.apply();                          // the first session starts with them
// ...
live.refresh();                        // when the host polls: fetch, verify, stage
if (live.pending()) live.apply();      // at a session start or in a lobby, never mid-match
live.rollback("balance_2026_10");      // stays off, here and after a restart
for (auto& o : live.live()) log(o.layer, o.version, o.origin);
```

- **Transport** is the host's: an interface with `fetch_manifest` and `fetch_overlay`. The
  host implements it over its HTTP client, a platform's title storage, or its game server's
  RPC. Dedicated servers use the same client and manifest.
- **Verify, before Baker sees a byte:** the manifest's signature with a key the game trusts,
  its environment and sequence, then each overlay's size and SHA-256.
- **Stage:** `check_layer_text` under the policy the manifest names. Nothing changes.
- **Apply:** only when the host calls `apply()`. It removes what the new set drops or
  replaces and pushes the rest under the same policy. Baker refuses what fails, changing
  nothing.
  - `ApplyMode::Path` (the default) pushes a file layer through a reader that serves the
    verified text by name (`liveops/NAME@VERSION.dta`). The pack's `requires` and the
    origin are read with it, so `source()` names the layer, its version (in the file name),
    and its origin. It needs a Config without `cache_dir` (requirements/0001).
  - `ApplyMode::Tree` reads the text as text only and pushes the tree: for a Config that
    loads through caches. `source()` names the layer and the file, without the origin.
- **Keep the last good:** after each apply, the set and its texts go to the `Store`. At start,
  `restore()` verifies and checks them again. One that no longer passes is dropped, and the
  game starts on the base without it.

## The publisher

```sh
baker-liveops keygen --out ~/.keys/liveops-prod.key       # prints the public key the game trusts
baker-liveops check   --game G --base CHECKOUT --overlay F --policy hotfix
baker-liveops publish --game G --base CHECKOUT --overlay F --policy hotfix \
                      --backend static:/srv/cdn --env prod --key ~/.keys/liveops-prod.key \
                      [--stack build-stacks/bin/stacks_ledger --stack-data CHECKOUT/data]
baker-liveops withdraw balance_2026_10 --backend static:/srv/cdn --env prod --key ...
baker-liveops show --backend static:/srv/cdn --env prod --trust PUBKEY
baker-liveops retire --game G --base CHECKOUT --overlay F --patch retire.patch
```

- **`check` and `publish`** run the client's own check against the base checkout, built as
  the game builds it. They refuse an overlay that declares no pack and version, names a
  policy the game lacks, or is refused by Baker.
  - With `--stack`, a baker-dev stack runs its scenarios on a copy of its directories:
    first without the overlay, then with it as the agent layer. If `compare` reports an
    objective that passed before and breaks with it, the publish is refused.
  - Then it signs and uploads: the overlay first (named by its content), the manifest last.
  - It publishes only on top of a manifest that verifies with its own key or one given with
    `--trust`.
- **Keys** stay out of repositories and logs: a key file (written 0600, never overwritten)
  or `LIVEOPS_SIGNING_KEY`. The publisher never prints a key; the tests make fresh ones in a
  temporary directory.
- **`retire`** writes the overlay's effect into the base as a unified diff. It loads the
  base with the overlay as a layer and runs `Config::promote` with a writer that captures the
  files, or it runs the stack's `promote --patch`. It writes nothing else and pushes nothing:
  a person applies the patch on a branch, reviews it, merges it, and withdraws the overlay
  once a build with it ships. If a value lives in a generated file (Ledger's `economy.dta`,
  from `economy.xlsx`), it says to change the source instead.

## Backends

- **`static:DIR`**: `ENV/manifest` and `ENV/overlays/NAME-HASH.dta` under a prefix. Upload
  the directory to any HTTPS host; a game fetches the same keys.
- **`cmd:PROGRAM`**: any store reached by a program that answers `get KEY` and `put KEY`.
  The Nakama backend is one:
  `cmd:python3 -I backends/nakama/liveops_nakama.py --url https://nakama.example`.
  - It publishes through the server-to-server RPC `liveops_put`, with the runtime's
    `http_key` (`NAKAMA_HTTP_KEY`).
  - Games read through `liveops_manifest` and `liveops_overlay` with a session; the
    script's `--client` reads the same way.
  - The objects are owned by the system user, one collection per environment, and clients
    cannot read them directly.
- A commercial store (a platform's title storage, a flag service) is a backend in a
  separate, private repository.

## Building and testing

```sh
sudo apt-get install libsodium-dev ninja-build      # or vcpkg / brew; -DSODIUM_ROOT=dir
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release [-DBAKER_SHIPPING=ON]
cmake --build build && ctest --test-dir build
```

Baker is fetched with FetchContent at baker-dev's main commit `75e1728`, which has the layer
check, until a release of `zach/baker` has it. To build offline, pass
`-DFETCHCONTENT_SOURCE_DIR_BAKER=path/to/baker-dev`. In a shipping build, the client and its
tests build, but the publisher does not, because `retire` needs a development build.

The tests use no network. A directory stands in for storage, and the keys are made for
each run. Optional runs:
- `LIVEOPS_STACK_BIN` and `LIVEOPS_LEDGER` add Ledger's scenario gate and both retire routes.
- The Nakama job runs the same end-to-end tests through `backends/nakama` against Nakama in a
  container (`docker-compose.yml`). See `.github/workflows/ci.yml`.

## Measured

`tests/bench.cpp` measures a 10 KB overlay that changes 256 values of a 2,000-item keyed table,
under a policy with every rule on. The numbers are medians of 500 runs in a shipping build, on
4 vCPUs of a 2.8 GHz Xeon, with GCC 13. All of it is main-thread time.

| | verify | stage (check) | apply, `Recheck::Always` | apply, `Recheck::IfStackChanged` |
|---|---|---|---|---|
| no validators | 0.11 ms | 1.14 ms | **1.29 ms** | **0.29 ms** |
| `shape::validator` | 0.11 ms | 1.43 ms | **1.87 ms** | **0.59 ms** |

- **The default misses the target.** Applying under 1 ms is not met by default. `apply()`
  pushes under the policy, as the plan asks, and Baker's policy check is about 1.4 ms of the
  push's 1.3–1.9 ms. The merge and rebind alone take 0.14 ms (0.45 ms with shapes).
- **The opt-in meets it.** `Recheck::IfStackChanged` skips the second check when the stack
  is the one the overlay was checked against: 0.3–0.6 ms. Requirement 0002 asks Baker to
  make that safe by construction.
- **Staging also lands on the main thread**, on the frame the overlay arrives, because the
  check needs the Config (requirement 0003).
- **Baker's own number** for the check alone was about 0.85 ms on a 2,000-item table.

## Requirements for Baker

When this integration needs something Baker lacks, write it down in `requirements/` from
`requirements/0000-template.md`, in Baker's own terms. Only the text of a requirement goes
to Baker.

- `0001` Push text a host received as a file layer, read as text only, with its pack and origin.
- `0002` Apply a layer that was checked, without checking it again.
- `0003` Check a candidate layer off the thread that owns the Config.

## License

This repository is licensed under the MIT License (`LICENSE`), as Baker is. Baker is used
here as a dependency; nothing in this repository is part of Baker. libsodium is ISC. Nakama
is a product of its owners, who have not endorsed this project; its server is Apache-2.0
and runs only in the tests' container.
