# Refactor2 Task 14 — Modularize MovieDetailsScreen

## Objective

Extract rendering and background artwork/data work from `MovieDetailsScreen.cpp`, leaving lifecycle/input coordination in the core file.

## Preconditions

- Task 13 commit exists.
- UI foundation, artwork/episode, and playback groups pass.

## Allowed Files

- `src/ui/screens/MovieDetailsScreen.cpp`, `MovieDetailsScreen.hpp`
- New `src/ui/screens/MovieDetailsRender.cpp`
- New `src/ui/screens/MovieDetailsWorker.cpp`
- Existing artwork/layout pure helpers only for removal of exact duplication
- UI foundation, artwork/episode, playback, and misc tests
- `Makefile`, `Makefile.cross`, `Makefile.desktop`

## Required result

- Move all drawing/layout/text-wrap/render definitions and exclusive constants to `MovieDetailsRender.cpp`.
- Move cache/network artwork loading or other background definitions and exclusive helpers to `MovieDetailsWorker.cpp`.
- Retain constructor/destructor, enter/leave, input, action selection, update, and navigation coordination in `MovieDetailsScreen.cpp`.
- Preserve exact layout, copy, controls, scrolling, playback/download actions, cached-first artwork, retry/error behavior, cancellation, and destruction ordering.
- If the current file has no separable background work, do not create an empty worker file; record that in the commit body and create only the render unit.

## Verification

Run UI foundation, artwork/episode, playback, downloads, and misc binaries, then:

```sh
make test -j2
make -j2
git diff --check
```

## Commit

```text
refactor(ui): split movie details concerns
```
