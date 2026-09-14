# Refactor2 Task 13 — Modularize SeriesScreen

## Objective

Turn `SeriesScreen.cpp` into a lifecycle/coordinator core by extracting rendering, input/navigation, and refresh/artwork worker definitions along existing responsibility lines.

## Preconditions

- Task 12 commit exists.
- UI foundation and artwork/episode groups pass.

## Allowed Files

- `src/ui/screens/SeriesScreen.cpp`, `SeriesScreen.hpp`
- New `src/ui/screens/SeriesScreenRender.cpp`
- New `src/ui/screens/SeriesScreenNavigation.cpp`
- New `src/ui/screens/SeriesScreenWorker.cpp`
- Existing shared pure UI helpers only when removing literal duplication
- UI foundation, artwork/episode, cache/offline, and misc regression tests
- `Makefile`, `Makefile.cross`, `Makefile.desktop`

## Required result

- Rendering/drawing/text/layout definitions and exclusive constants go to `SeriesScreenRender.cpp`.
- Input handling, focus transitions, and selection movement go to `SeriesScreenNavigation.cpp`.
- refresh/fetch/artwork worker definitions and exclusive worker helpers go to `SeriesScreenWorker.cpp`.
- Constructor/destructor, enter/leave, update coordination, and state ownership remain in `SeriesScreen.cpp`.
- Preserve pixels/layout, strings, controls, selection, sorting, page/window semantics, cached-first display, API order, offline behavior, worker bounds, cancellation/wakeup/join ordering, and screen-owned-state lifetime.
- Move definitions first; do not redesign screen state or create a base-screen framework.

## Verification

Run UI foundation, artwork/episode, cache/offline, and misc binaries, then:

```sh
make test -j2
make -j2
git diff --check
```

STOP if an extraction would introduce cross-screen inheritance or alter worker ownership.

## Commit

```text
refactor(ui): split series screen concerns
```
