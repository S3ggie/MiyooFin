# Code quality

## Required checks

From the repository root, run `make ci-local` before normal pushes. It runs
the authoritative clang-format-18 check, host build, tests and boundary checks,
ASan+UBSan tests, scripted UI tests, and `git diff --check`:

```shell
make ci-local
```

Before substantial or refactor work, run the superset, which also performs the
ARM cross-build and `make verify-arm`:

```shell
make ci-local-full
```

The aggregate checks require `clang-format-18`, `xvfb-run`, and (for the full
run) Docker plus the `miyoofin-toolchain` image. Missing required tools are
validation failures. `make format-check` retains its narrow fallback for
minimal environments, but that fallback must not be claimed as CI-equivalent;
`ci-local` always preflights and invokes `clang-format-18` explicitly. The
format check covers tracked first-party C and C++ files under `src/`,
`include/`, `tools/`, and `tests/`, while excluding vendored/imported code,
generated output, and non-C/C++ tool scripts.

Host and ARM C++ builds use `-Wall -Wextra -Wpedantic`. The host build's only
intentional warning exception is `-Wno-unused-parameter` for the vendored
`src/image/stb_image_impl.cpp` translation unit. First-party warnings should
be fixed rather than suppressed globally.

## ThreadSanitizer (host concurrency)

`make test-tsan` runs the concurrency-heavy host suites under
ThreadSanitizer:

```shell
make test-tsan
```

It builds into its own `output/tsan/` tree (separate objects and binaries) with
`-fsanitize=thread -fno-omit-frame-pointer -O1 -g`, so it never reuses stale
non-TSan objects from `make test` or `make test-sanitize`. Both first-party
translation units and the vendored `vendor/sqlite/sqlite3.c` host object are
instrumented; SDL2, libcurl, and other system libraries are external and
uninstrumented, so a race whose both sides live entirely inside them cannot be
reported. `SANITIZE=1` and `TSAN=1` are mutually exclusive: ASan and TSan cannot
instrument the same binary, and the Makefile rejects the combination.

The runner is serial and focused on `test_library_coordinator`,
`test_library_hierarchy`, `test_catalog`, `test_home_library_controller`,
`test_home_artwork_controller`, and `test_downloads`. It fails on any non-zero
exit (TSan exits 66 on a report) and on any `ThreadSanitizer` marker in a log,
and keeps per-binary logs under `output/tsan/test/logs/` on failure. There are
no suppressions: a first-party race must be fixed, not silenced. An
external-only report, if one is ever unavoidable, must be classified and
documented narrowly rather than globally suppressed.

This target is host-only and intentionally separate from `make ci-local` and
`make ci-local-full`; it needs a TSan-capable compiler and an instrumented
rebuild. CI runs it in a dedicated job (`.github/workflows/ci.yml`). On hosts
where TSan reports "unexpected memory mapping", lower `vm.mmap_rnd_bits` (for
example `sudo sysctl -w vm.mmap_rnd_bits=28`) or run under `setarch -R`.

When changing thread ownership, worker lifetime, cancellation, or publication
ordering, run `make test-tsan` in addition to `make ci-local`.

## Optional clang-tidy

`make clang-tidy` is an opt-in, host-only analysis. It requires `clang-tidy`
and [Bear](https://github.com/rizsboa/bear). Bear first regenerates
`compile_commands.json` from `make clean all`, then clang-tidy analyzes the
host production source list using the conservative checks in `.clang-tidy`.
The compilation database is machine-specific generated output and is not
tracked. No clang-tidy result is required for the ARM build; ARM validation
remains the Docker cross-build and `make verify-arm` path.

The target does not enable warnings-as-errors, rewrite files, or run as part of
the required CI path. This keeps the opt-in analysis useful without changing
the build's diagnostics or behavior.
