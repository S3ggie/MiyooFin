# Refactor2 Autonomous Modularization Roadmap

## Goal

Reduce the remaining large compilation and ownership units without changing MiyooFin behavior, persistence semantics, network behavior, or the hardware-verified download/playback architecture.

This roadmap is deliberately ordered so one Luna High agent can execute every task sequentially. Start with `tasks/refactor2-task1.md`, follow `EXECUTION_RULES.md`, and continue through Task 16 without asking between successful tasks.

## Ready-to-use agent instruction

Give the implementation agent this exact instruction:

> Execute the entire Refactor2 roadmap autonomously. First read the repository-root `AGENTS.md`, then `Refactor2/EXECUTION_RULES.md` and `Refactor2/README.md`. Start at the first incomplete numbered task in `Refactor2/tasks/` and continue sequentially through Task 16. Follow every Allowed Files list, invariant, validation command, STOP condition, and exact commit boundary. Do not ask between successful tasks. Stop immediately and report evidence when the execution rules require it. Do not push or deploy.

## Baseline audited

- Git HEAD at planning time: `63efe3b`.
- The worktree was clean at planning time.
- `src/catalog/CatalogDb.cpp` is about 5,071 lines.
- `src/app/App.cpp` and `src/diagnostics/PerformanceTelemetry.cpp` are each about 1,060 lines.
- `src/ui/screens/HomeScreenSync.cpp`, `SeriesScreen.cpp`, and `src/library/LibrarySync.cpp` are about 800–880 lines each.
- Several already-split test groups exceed the repository's roughly 1,000-line threshold: telemetry, catalog parity, and API/session.

## Ordered task index

- [x] 1. [Split telemetry tests](tasks/refactor2-task1.md)
- [x] 2. [Split catalog parity tests](tasks/refactor2-task2.md)
- [x] 3. [Split API/session tests](tasks/refactor2-task3.md)
- [x] 4. [Extract CatalogDb schema and migration implementation](tasks/refactor2-task4.md)
- [x] 5. [Extract CatalogDb write and transaction implementation](tasks/refactor2-task5.md)
- [x] 6. [Extract CatalogDb query implementation](tasks/refactor2-task6.md)
- [x] 7. [Extract CatalogDb sync-state and hierarchy implementation](tasks/refactor2-task7.md)
- [x] 8. [Modularize LibrarySync incremental synchronization](tasks/refactor2-task8.md)
- [x] 9. [Modularize Home synchronization projections](tasks/refactor2-task9.md)
- [x] 10. [Modularize App session lifecycle](tasks/refactor2-task10.md)
- [x] 11. [Modularize App external playback lifecycle](tasks/refactor2-task11.md)
- [x] 12. [Modularize performance telemetry](tasks/refactor2-task12.md)
- [x] 13. [Modularize SeriesScreen](tasks/refactor2-task13.md)
- [x] 14. [Modularize MovieDetailsScreen](tasks/refactor2-task14.md)
- [x] 15. [Finish EpisodeBrowserScreen coordinator extraction](tasks/refactor2-task15.md)
- [x] 16. [Enforce boundaries and update architecture documentation](tasks/refactor2-task16.md)

Tasks 1–3 improve focused feedback. Tasks 4–7 split the largest persistence unit without changing its public class. Tasks 8–15 isolate orchestration concerns while retaining existing owners and threading. Task 16 locks in the resulting boundaries.

## Completion definition

All 16 task commits exist in order; every task's focused validation and final validation passed; `make test -j2`, `make -j2`, and `git diff --check` pass at the final HEAD; no runtime/generated files were committed; nothing was deployed; and the final report names any hardware behavior that remains unverified.
