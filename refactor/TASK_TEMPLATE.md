# Task NNN — Title

## Status
NOT STARTED

## Depends On
Task NNN

## Goal
One concrete behavior-preserving change.

## Why
Why this seam is being created.

## Allowed Files
- exact/path

## Forbidden Scope
Everything not listed above.

## Pre-change checks
```sh
git status --short
make test
```

## Exact Steps
1. ...
2. ...

## Behavior That Must Not Change
- ...

## Focused Validation
```sh
...
```

## Required Final Validation
```sh
make test
git diff --check
git status --short
git diff --stat
git diff
```

## Acceptance Criteria
- ...

## Commit
`refactor: ...`

## STOP
After committing, STOP. Do not begin the next task.
