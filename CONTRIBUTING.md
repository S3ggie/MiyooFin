# Contributing to MiyooFin

Bug reports, documentation improvements, and focused pull requests are welcome.
MiyooFin targets the Miyoo Mini Plus running OnionOS. Read the
[README](README.md) for setup and the [architecture guide](docs/architecture.md)
for subsystem responsibilities.

## Report a problem or suggest an improvement

Search [existing issues](https://github.com/S3ggie/MiyooFin/issues) first.
For a bug, use the [bug-report form](https://github.com/S3ggie/MiyooFin/issues/new?template=bug_report.yml)
and include:

- MiyooFin, OnionOS, and Jellyfin server versions.
- Whether the problem occurs in video or music, online or offline.
- Steps to reproduce, expected behavior, and actual behavior.
- Relevant screenshots or a crash report from **Settings → Diagnostics**, when available.

Remove passwords, access tokens, authenticated URLs, and personal information
from screenshots and logs before posting them. For feature suggestions,
explain the use case and the behavior you would like to see.

## Prepare a pull request

1. Fork the repository, create a branch, and keep changes focused on one problem.
2. Follow the [host setup](README.md#host-development-linux) or
   [cross-build instructions](README.md#onionos-cross-build).
3. Read [AGENTS.md](AGENTS.md) for detailed project rules, including worker
   ownership, local-first behavior, test organization, and resource limits.
4. Update affected documentation and add focused regression coverage for
   behavior changes. Documentation-only changes need link, example, and
   whitespace checks.
5. Describe the problem, resulting behavior, validation actually run, and any
   remaining hardware or environment limitations in the pull request.

Do not include runtime files, account data, downloads, or generated build output.
Keep reviews and discussions respectful and specific.

## Validate code changes

The required host check before a normal code push is:

```shell
MIYOOFIN_JOBS=2 make ci-local
```

It covers authoritative formatting, the host build, tests, boundary checks,
ASan/UBSan, scripted UI flows, and `git diff --check`.
For substantial or refactor changes, use `make ci-local-full`, which adds
ARM cross-build verification. Changes to concurrency also need
`make test-tsan`; runtime or playback changes need the additional checks
listed in [AGENTS.md](AGENTS.md).

The aggregate host checks require `clang-format-18`, Python 3, and `xvfb-run`,
in addition to the build dependencies. Full checks also need Docker and the
Miyoo toolchain image. Missing tools are not a passing result. See
[code quality](docs/code-quality.md) and [toolchain setup](docs/toolchain.md).

Use `make format` and `make format-check` for first-party C/C++ files.
Do not run bulk formatting over vendored, imported, or generated sources.
On a shared Linux host, contain heavy jobs with the repository wrapper:

```shell
MIYOOFIN_JOBS=2 MF_MEM=8G MF_TIME=1500 tools/bounded.sh make ci-local
```

The wrapper needs a working user systemd session and refuses to run without
resource containment. Keep compile parallelism bounded. Host tests and an ARM
build do not prove on-device behavior; state explicitly which hardware checks
you performed. Do not run GPU or Jellyfin playback stress tests on a shared host.

## License

Contributions are distributed under the project's [GPL-3.0-only license](LICENSE).
For imported or bundled material, preserve its license and update
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) when applicable.
