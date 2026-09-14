# Refactor2 Task 10 — Modularize App session lifecycle

## Objective

Move saved-session validation/loading, catalog scope configuration, login/home routing, and logout lifecycle out of `App.cpp` into `AppSession.cpp` without changing startup behavior.

## Preconditions

- Task 9 commit exists.
- Baseline: API/session, catalog, and UI foundation groups pass.

## Allowed Files

- `src/app/App.cpp`, `App.hpp`
- New `src/app/AppSession.cpp`
- `src/app/Session.*` only if include cleanup is required
- API/session, catalog, UI foundation, and misc regression tests
- `Makefile`, `Makefile.cross`, `Makefile.desktop`

## Required result

- Move existing definitions for saved URL/session loading and validation, catalog scope setup/teardown, `goToHome`, `goToLogin`, and `logout`, plus exclusive file-local helpers.
- Keep construction, SDL initialization, main loop, shutdown, and playback lifecycle in `App.cpp` for now.
- Preserve screen-stack order, messages, credential/session persistence, validation threading, catalog scope ownership, offline fallback, and logout cleanup order.
- Never log tokens or authenticated URLs. No synchronous validation/network work on the UI thread.
- No public signature or data-format changes.

## Verification

Run API/session, catalog, UI foundation, and misc binaries, then:

```sh
make test -j2
make -j2
git diff --check
```

STOP if current initialization ordering cannot be preserved exactly.

## Commit

```text
refactor(app): extract session lifecycle
```
