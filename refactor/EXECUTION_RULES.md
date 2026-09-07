# Refactor Execution Rules

These rules apply to every numbered refactor task.

## Hard invariants

- This is a behavior-preserving refactor unless a task explicitly says otherwise.
- No new user-visible features.
- No protocol changes.
- No cache-format, download-manifest, session-format, or playback-request format changes.
- No TLS verification weakening.
- No access token may be added to artwork URLs or playback request files.
- Preserve canonical server identity and current LAN/public fallback semantics.
- Preserve offline browsing and locally playable download behavior.
- Preserve current HLS retry/cancellation behavior.
- Preserve existing worker ownership and cancellation behavior unless the task explicitly moves it.
- The SDL/UI thread must not gain blocking HTTP, filesystem, removable-storage, JPEG decode, or worker-join work.
- Do not add third-party dependencies.
- Do not rewrite working code merely for style.
- Do not mass-format files.
- Do not rename public behavior-facing concepts unless the task explicitly requires it.

## Scope discipline

- Modify only files listed under `Allowed Files`.
- `Allowed Files` is a hard boundary, not a suggestion.
- If an additional file is genuinely required, STOP and report the dependency.
- Never modify `refactor/tasks/*.md`.
- Never combine two numbered tasks into one commit.
- Never start the next task automatically.

## Git discipline

Before edits:

```sh
git status --short
```

The output must be empty.

After edits:

```sh
git diff --check
git status --short
git diff --stat
git diff
```

Review the complete diff before committing.

Do not amend older commits. Do not squash tasks while executing the plan.

## Validation discipline

Every task must run its listed focused validation plus:

```sh
make test
```

unless the task explicitly documents why an earlier compile-only checkpoint is used in addition
to, rather than instead of, `make test`.

A task is not complete because it "looks correct." It is complete only when its mechanical
acceptance criteria and required commands pass.

## Failure behavior

If the untouched pre-change tree fails a required check:
- STOP.
- Do not edit code.
- Report the exact failing command and output.

If validation fails after the change:
- Diagnose only the current task.
- Fix only current-task regressions.
- Do not widen into unrelated refactoring.

If the requested extraction exposes hidden coupling:
- Prefer leaving the coupled code in its original file.
- STOP if the task cannot remain behavior-preserving inside its allowed scope.

## Mechanical extraction rule

Many tasks split a large `.cpp` file without changing class design. For these tasks:
- Move function bodies byte-for-byte except for include adjustments required to compile.
- Keep namespaces identical.
- Keep function signatures identical.
- Keep static/local helper semantics identical.
- Do not reorder statements.
- Do not "simplify" expressions.
- Do not change locking, atomics, cancellation checks, timeouts, or worker wakeups.
