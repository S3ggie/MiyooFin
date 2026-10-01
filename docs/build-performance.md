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
