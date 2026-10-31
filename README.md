# Baker for live ops

An integration of [Baker](https://github.com/zach/baker) with the places a live game keeps
its overlays. It publishes, delivers, and retires *published overlays*: versioned, checked
layers that change a shipped game's data without a client patch, applied at a boundary
the game chooses, rolled back in one call, and promoted into source once they stick.

It follows plan D of baker-dev's `dev/integration-plans.md` (the refined plan) and the
convention in Baker's cookbook, "Published overlays" (D162), on Baker's layer check
(`Config::check_layer_text`, `LayerPolicy`, `LayerOptions::policy`, D161).

| Directory | What |
|---|---|
| `client/` | The C++20 helper a game links: verify, stage, apply at the host's boundary, keep the last good, roll back, report what is live. libsodium; no vendor SDK. |
| `publish/` | `baker-liveops`, the command line: `check`, `publish`, `withdraw`, `show`, `keygen`, and `retire` (`retire/`). |
| `backends/static/` | Files under a prefix: any HTTPS host, CDN or bucket. A directory stands in for it in the tests. |
| `backends/nakama/` | Nakama storage objects per environment, served by runtime RPCs (a Lua module), and the publisher's command for it. |
| `tests/` | The client's tests, the end-to-end tests, the timings, a test game, and Ledger's description. |
| `requirements/` | What this integration needs from Baker, in Baker's terms. |

## The convention

**An overlay is a pack.** Its `#pack` names it, its version, and the base it was made for;
its `#origin` says where it came from, so `source()` names it:

```dta
#pack (balance_2026_10 (level live) (version "3") (requires (game_data "1.4")))
#origin ("ops/balance_2026_10.dta" (tool baker-liveops))
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
cfg.push_layer("default", "data/default.dta");
cfg.freeze();                          // ConfigOptions::live_layers = true
live.restore();                        // the last good overlays, verified and checked again
live.apply();                          // the first session starts with them
// ...
live.refresh();                        // when the host polls: fetch, verify, stage
if (live.pending()) live.apply();      // at a session start or in a lobby, never mid-match
live.rollback("balance_2026_10");      // stays off, here and after a restart
for (auto& o : live.live()) log(o.name, o.version, o.origin);
```

- **Transport** is the host's: an interface with `fetch_manifest` and `fetch_overlay`. The
  host implements it over its HTTP client, a platform's title storage, or its game server's
  RPC. Dedicated servers use the same client and manifest.
- **Verify, before Baker sees a byte:** the manifest's signature with a key the game trusts,
  its environment and sequence, then each overlay's size and SHA-256.
- **Stage:** Baker checks the verified text under the policy the manifest names, as received
  text (never a cache, no `#include`), changing nothing. To keep that off the frame, split
  `refresh()`: `begin_refresh()` on the Config's thread takes a `LayerChecker`, `run()` on
  any thread fetches, verifies and checks, and `finish_refresh()` on the Config's thread
  stages. Validators added with `{.any_thread = true}` run in the check; the push runs the
  rest.
- **Apply:** only when the host calls `apply()`. It removes what the new set drops or
  replaces, and pushes the rest with `push_layer_text` under the same policy and the staged
  report (`.checked`). If the Config has not changed since the check, Baker takes the
  report's verdict; otherwise it checks again and refuses what fails, changing nothing.
  - Each overlay's layer is `liveops/NAME@VERSION.dta`, so `source()` names the overlay and
    its version, and the pack's `requires` and its `#origin` are read with it.
- **Bound the names an overlay adds:** names live until the program ends, so each verified
  overlay is read once under `ClientOptions::max_new_symbols` (16,384 by default) before
  the check. Baker's received-text reads do not apply that cap yet (requirements/0005).
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
  files, or it runs the stack's `promote --baker-patch`. It writes nothing else and pushes
  nothing: a person applies the patch on a branch, reviews it, merges it, and withdraws the
  overlay once a build with it ships. If a value lives in a generated file (Ledger's `economy.dta`,
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

Baker is fetched with FetchContent at its release `v0.7.0`. To build offline, pass
`-DFETCHCONTENT_SOURCE_DIR_BAKER=path/to/baker`.
In a shipping build, the client and its tests build, but the publisher does not, because
`retire` needs a development build.

The tests use no network. A directory stands in for storage, and the keys are made for
each run. Optional runs:
- `LIVEOPS_STACK_BIN` and `LIVEOPS_LEDGER` add Ledger's scenario gate and both retire routes.
- The Nakama job runs the same end-to-end tests through `backends/nakama` against Nakama in a
  container (`docker-compose.yml`). See `.github/workflows/ci.yml`.

## Measured

`tests/bench.cpp` measures a 10 KB overlay that changes 256 values of a 2,000-item keyed table,
under a policy with every rule on. The numbers are medians of 500 runs in a shipping build,
with Baker 0.7.0 and GCC 13, on 4 vCPUs of a shared 2.8 GHz Xeon container.

| | verify | check (any thread) | apply, report holds | apply, stack moved |
|---|---|---|---|---|
| no validators | 0.10 ms | 0.70 ms | **0.23 ms** | **0.65 ms** |
| `shape::validator` | 0.10 ms | 0.88 ms | **0.51 ms** | **1.19 ms** (p90 1.3 ms) |

- **Apply is the main thread's cost at the boundary.** When the stack has not moved since
  staging, the push takes the staged verdict: 0.2–0.5 ms. When it has, Baker checks again,
  and with shape validation that passes 1 ms here: the case to avoid by staging close to the
  boundary.
- **The check runs on the worker.** Verifying and checking take about 0.8–1.0 ms off the
  main thread, including the client's capped read of the overlay (requirements/0005).
  Taking the checker and staging take under 0.01 ms on it.
- **Hosts differ.** The same Baker measured about 15% faster on a quieter machine; measure
  on the target hardware.

## Requirements for Baker

When this integration needs something Baker lacks, write it down in `requirements/` from
`requirements/0000-template.md`, in Baker's own terms. Only the text of a requirement goes
to Baker.

- **In Baker since 0.1.0** (baker-dev D163–D167), and the client uses them:
  - `0001` Push text a host received as a file layer, read as text only, with its pack and
    origin: `Config::push_layer_text`.
  - `0002` Apply a layer that was checked, without checking it again:
    `LayerOptions::checked`.
  - `0003` Check a candidate layer off the thread that owns the Config: `Config::checker()`.
- **Open, from Baker 0.7.0:**
  - `0004` A layer's effect lists the values it changes, apart from what it declares.
  - `0005` Cap the names a received layer may add, apart from the game's own data. The
    client works around it.

## License

This repository is licensed under the MIT License (`LICENSE`), as Baker is. Baker is used
here as a dependency; nothing in this repository is part of Baker. libsodium is ISC. Nakama
is a product of its owners, who have not endorsed this project; its server is Apache-2.0
and runs only in the tests' container.
