# Code quality

## Required checks

From the repository root, run the following before submitting a first-party
change:

```shell
make format-check
make -j2
make test -j2
make refactor-check
git diff --check
```

`make format-check` is authoritative whenever `clang-format` is installed. It
covers tracked first-party C and C++ files under `src/`, `include/`, `tools/`,
and `tests/`, while excluding vendored/imported code, generated output, and
non-C/C++ tool scripts. Minimal environments without clang-format retain a
deliberately narrow fallback for obvious same-line control-flow and statement
regressions; that fallback is not a replacement for clang-format. CI pins the
formatter to the Ubuntu `clang-format-18` package and invokes it as
`clang-format-18`; use `CLANG_FORMAT=clang-format-18 make format-check` locally
when matching CI.

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
