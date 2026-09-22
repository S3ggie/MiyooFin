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
