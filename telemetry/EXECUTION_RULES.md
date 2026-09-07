# Execution Rules

- Execute exactly one numbered task.
- One task = one commit = STOP.
- Read `AGENTS.md`, this file, `ARCHITECTURE.md`, `SECURITY.md`, `SCHEMA_V1.md`, and the task.
- Work directly; do not spawn subagents unless a human explicitly authorizes it.
- `Allowed Files` is a hard boundary. If another path is needed, STOP.
- Unexpected dirty worktree or failing baseline means STOP without commit.
- Preserve unrelated work.

## Build-variant rule

MiyooFin reuses the same object/output paths across `PERF_TELEMETRY` values. Before changing the value between 0 and 1, run:

```sh
make clean
```

A compile-out/on validation without that clean rebuild is invalid.

## New production `.cpp`

Any task creating a new production `.cpp` must include and update **both** `Makefile` and `Makefile.cross`, add appropriate diagnostics output directories, and validate ARM.

## Disabled-path performance

Compile-out must eliminate telemetry timing/context/event work from hot paths. Runtime-off must check `enabledFast()` before any telemetry `clock_gettime`, TLS context mutation, or event work. Frame/cache/decode/network/HLS tasks must inspect this explicitly.

## Producer rules

Producers never write trace files, read `/proc`, call `statvfs`, block for telemetry, allocate dynamic per-event trace payloads, or serialize strings. Ring pressure drops telemetry.

## Measurement ownership

- workers: activity, queue, completion/failure/cancel, semantic context;
- ImageCache: generic cache probe/read/write aggregates;
- ImageDecoder: generic decode timing/input/output and ArtworkDecode;
- JellyfinApi: RequestKind only;
- RouteRequest: RouteKind/attempt/fallback only;
- HttpClient: normal/binary transport records;
- HLS transfer code: direct HLS attempt/retry records;
- service thread: periodic aggregate emission.

Before commit always run `git diff --check`, `git status --short`, `git diff --stat`, `git diff` and review the complete diff.

STOP without commit if scope is insufficient, architecture/security would change, validation cannot be repaired inside scope, or a hardware task lacks the real device.
