# Task 019 — Split EpisodeBrowser download actions/planning

## Status

NOT STARTED

## Depends On

Task 018

## Goal

Split EpisodeBrowser download actions/planning. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/EpisodeBrowserScreen.cpp`
- `src/ui/screens/EpisodeBrowserScreen.hpp`
- `src/ui/screens/EpisodeBrowserDownloads.cpp`
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

Create `EpisodeBrowserDownloads.cpp`.

The current Episode/Season download logic may be inline inside `EpisodeBrowserScreen::handleAction()`
and/or `update()`. That is expected and is NOT a STOP condition.

Extract only the existing download-specific inline blocks into the smallest reasonable set of private
`EpisodeBrowserScreen` member methods.

Add the required private method declarations to `EpisodeBrowserScreen.hpp`, and place their
definitions in `EpisodeBrowserDownloads.cpp`.

The extracted methods may cover only existing behavior for:
- Episode download actions;
- Season download actions;
- download-plan request/polling;
- download-plan confirmation;
- stale plan-generation protection;
- existing DownloadManager interaction directly associated with those actions.

Replace the original inline blocks with calls to the extracted methods.

Preserve the original conditions, statement order, state mutations, plan generations, confirmation
behavior, and return behavior as closely as possible. This is extraction, not redesign.

Do not modify DownloadManager itself.
Do not combine Episode and Season actions.
Do not change download persistence or worker behavior.

## Behavior That Must Not Change

- Episode and Season download controls remain separate.
- Plan acceptance/generation behavior stays identical.
- No storage I/O moves onto SDL thread.

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
git add src/ui/screens/EpisodeBrowserScreen.cpp src/ui/screens/EpisodeBrowserScreen.hpp src/ui/screens/EpisodeBrowserDownloads.cpp Makefile
git commit -m "refactor: split episode downloads"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
