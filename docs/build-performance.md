# Build performance

Measured on an 8-core machine, from a completely cold tree (no caches):

| What | Before | Now |
|------|-------:|----:|
| `make ci-local` (host build, tests, ASan+UBSan, UI flows) | 950 s | 433 s |
| ARM cross-build (`make onionos`) | ~280 s | 66 s |
| ARM rebuild after touching one `.cpp` | ~280 s | 3 s |
| ARM rebuild after touching `HomeScreen.hpp` | ~280 s | 11 s |
| Host build, warm `ccache` | 52 s | 4 s |
| `docker build` step inside `make onionos` | 13 s | 1 s |

## What changed and why

- **Parallelism.** The ARM build ran one compile at a time inside Docker, and
  `ci-local` used `-j2` for the build and no `-j` for the test step. All of them
  now default to every core. Override with `JOBS=N` (`make onionos`) or
  `MIYOOFIN_JOBS=N` (the CI scripts).
- **Precompiled headers** (`src/pch.hpp`). Every translation unit re-parsed about
  155,000 preprocessed lines of standard-library, SDL and curl headers, roughly
  2 s of each compile. Each tree (production, test library, test binaries, ASan,
  TSan, ARM) builds a PCH with its own exact flags. The test PCH also contains
  `tests/test_support.hpp`, so it records its project-header dependencies with
  `-MMD` and is rebuilt when any of them changes. GCC ignores an invalid PCH and
  falls back to the plain header, and `-Winvalid-pch` reports that, so a PCH can
  only change speed. `USE_PCH=0` disables it.
- **`-g1` instead of `-g`** for host, ASan and TSan builds. Line tables are
  enough for sanitizer and assert traces and cost far less to compile and link.
  Use `DEBUG_FLAG=-g` for full DWARF when stepping in a debugger.
- **ARM objects are reused.** Compile flags (`RELEASE`, `PERF_TELEMETRY`) are not
  tracked by the header dependencies, so `output/build-arm/.config` records them
  and only a real configuration change rebuilds. Do not `rm -rf output/build-arm`
  between builds any more. The 9.5 MB SQLite amalgamation (about 28 s on its own)
  has fixed flags and is never rebuilt by a config change.
- **`.dockerignore`.** The Docker build context was the whole repository,
  including multi-GB `output/` trees. Only `vendor/miyoo/lib` and the
  developer-ssh helper script are sent now. The legacy builder reads only this
  root file, so it is not per-Dockerfile.
- **`ccache`** is used automatically when installed (`USE_CCACHE=0` to skip). CI
  restores `.ccache` between runs. It matters for fresh checkouts and for
  switching branches, where incremental builds cannot help.

## Where the time still goes

About 400 CPU-seconds go to 109 C++ files, and the `ui/screens/*` files include
`HomeScreen.hpp`, which pulls in most of the UI. Shrinking that header with
forward declarations is the next lever. `sqlite3.c` is a single 28 s unit that
cannot be split, which sets a floor for a cold ARM build.

## Day-to-day (warm trees): where `make ci-local` time goes

With nothing recompiling, a run was 130 s and almost all of it was waiting, not
compiling. After tuning it is 70 s:

| Phase | Before | Now |
|-------|-------:|----:|
| `clang-format` over all first-party files | 13 s | 4 s |
| Plain test suite (binaries + script tests + checks) | 36 s | 25 s |
| ASan+UBSan test suite | 39 s | 18 s |
| UI flows and offline harness tests | 42 s | 22 s |
| **Total** | **130 s** | **70 s** |

- `make format-check` runs one `clang-format` per file on every core (it was serial).
- Script-only tests (runner, CA bundle, launcher, module boundaries, telemetry
  decoder) do not exercise compiled code, so they run in the plain `make test`
  only, not again under ASan/TSan.
- `tests/test_playback_runner.sh` runs its independent cases concurrently, and
  overlaps them with its forced-exit lifecycle test (18.8 s -> 7 s).
- The launcher round-trip test no longer sleeps through the real restore backoff
  (`MIYOOFIN_RESTORE_BACKOFF_S`, default 2 s on the device path); 8.5 s -> 0.5 s.
- `tools/ui-script/run-all.sh` runs the offline remote-scenario test alongside the
  UI flows. The flows stay sequential because they share a `/tmp` screenshot
  flag file.

## `make ci-quick`

Format, host build, tests and boundary checks, without sanitizers, UI flows or
ARM: about 30 s warm. Use it between edits, and run `make ci-local` before a push.
Focused test binaries (`make output/test/test_ui_models`) are faster still.

What is left in the 70 s is mostly the slowest test binaries
(the `test_library_coordinator*` and `test_downloads*` groups, now several smaller binaries running in parallel),
the serialised timing-sensitive tests, and the UI flows (`series` 7 s,
`login-400` 10 s) that wait on a real app and stub server.
