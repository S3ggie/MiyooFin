# Task 018 — Split EpisodeBrowser playback handoff

## Status

NOT STARTED

## Depends On

Task 017

## Goal

Split EpisodeBrowser playback handoff. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/EpisodeBrowserScreen.cpp`
- `src/ui/screens/EpisodeBrowserScreen.hpp`
- `src/ui/screens/EpisodeBrowserPlayback.cpp`
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

Create `EpisodeBrowserPlayback.cpp`.

The current playback logic is inline inside `EpisodeBrowserScreen::handleAction()` and/or `update()`.
That is expected and is NOT a STOP condition.

Extract only the existing playback-specific inline blocks into the smallest reasonable set of private
`EpisodeBrowserScreen` member methods.

Add the required private method declarations to `EpisodeBrowserScreen.hpp`, and place their
definitions in `EpisodeBrowserPlayback.cpp`.

The extracted methods may cover only existing behavior for:
- initiating playback for the selected episode;
- choosing the existing local/downloaded versus online playback path;
- creating/triggering the existing external playback handoff;
- playback-specific artwork/prefetch pause/resume handling;
- consuming/applying the existing playback result after returning.

Replace the original inline blocks with calls to those extracted methods.

Preserve the original conditions, statement order, state mutations, return behavior, and playback
semantics as closely as possible. This is extraction, not redesign.

Do not modify PlaybackRequest, OfflinePlaybackJournal, runner scripts, routing implementation, or
files outside Allowed Files.

## Behavior That Must Not Change

- Playback request remains token-free.
- Resume ticks behavior is unchanged.
- Offline journal behavior is unchanged.
- External playback flag semantics are unchanged.

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
git add src/ui/screens/EpisodeBrowserScreen.cpp src/ui/screens/EpisodeBrowserScreen.hpp src/ui/screens/EpisodeBrowserPlayback.cpp Makefile
git commit -m "refactor: split episode playback"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
