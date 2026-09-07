# Task 017 — Split EpisodeBrowser artwork/prefetch worker

## Status

NOT STARTED

## Depends On

Task 016

## Goal

Split EpisodeBrowser artwork/prefetch worker. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/EpisodeBrowserScreen.cpp`
- `src/ui/screens/EpisodeBrowserArtwork.cpp`
- `Makefile`

## Forbidden Scope

Every file not listed under **Allowed Files** is forbidden for this task.
Do not edit other task files. Do not perform opportunistic cleanup.

## Pre-change Checks

Run these before editing:

```sh
git status --short
make test
```

`git status --short` must be empty. If either check fails, STOP without editing.

## Exact Steps

Create `EpisodeBrowserArtwork.cpp`.
Move artwork preparation, selected thumbnail loading, prefetch scheduling/worker, cancellation, and resume-after-playback artwork methods.
Keep existing generation/cancel rules and the policy that only selected artwork is decoded into RAM while nearby items warm disk cache.

## Behavior That Must Not Change

- Prefetch bounds remain identical.
- Selection change still preempts stale work.
- Playback still pauses/resumes prefetch as before.
- JPEG decode stays off SDL thread.

## Focused Validation

Run:

```sh
`make refactor-check`
```

## Required Final Validation

Run all of the following after the change:

```sh
make test
git diff --check
git status --short
git diff --stat
git diff
```

Read the full diff. Verify every changed path is listed under Allowed Files.

## Acceptance Criteria

- The stated extraction/change is complete.
- No unrelated behavior change is present.
- All required validation passes.
- Only Allowed Files changed.
- The worktree is clean after the commit.

## Commit

Commit exactly this task with:

```sh
git add src/ui/screens/EpisodeBrowserScreen.cpp src/ui/screens/EpisodeBrowserArtwork.cpp Makefile
git commit -m "refactor: split episode artwork worker"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
