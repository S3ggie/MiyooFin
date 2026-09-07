# Runner Prompts

## One task

```text
Work on MiyooFin telemetry.
Read AGENTS.md, telemetry/EXECUTION_RULES.md, ARCHITECTURE.md, SECURITY.md, SCHEMA_V1.md, and exactly one requested task.
Execute ONLY that task. Do not spawn subagents unless I explicitly authorize it. Allowed Files are a hard boundary.

MiyooFin shares output/build, output/test, and output/build-arm across PERF_TELEMETRY values. If the task switches 0↔1, run make clean before each switch exactly as written. Never report stale-object validation.

Compile-out must eliminate telemetry work. Runtime-off must check enabledFast before telemetry clock/context/timing work.

Run all validation, review the complete diff, commit exactly once with the task message, verify worktree, report SHA, STOP.
On any STOP condition: do not widen scope, do not commit, do not start the next task.
```

## Authorized range

```text
Execute only the explicitly authorized inclusive task range in numeric order. Recheck worktree before each task. Do not skip failures. Obey clean rebuilds on compile-variant switches. Commit once per task. Do not cross a REVIEW_CHECKPOINTS.md gate without explicit human approval.
```

Hard STOP: dirty worktree, baseline failure, insufficient Allowed Files, changed repository ownership/path, stale compile-variant validation, security/architecture conflict, unexpected concurrency/HLS/playback coupling, missing host/ARM source wiring, review gate without approval, or unavailable physical device for a hardware task.
