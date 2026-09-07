# Reviewer Checkpoints

The implementation model should not make architecture decisions. A stronger reviewer should inspect
the repository at these checkpoints before continuing:

## Checkpoint A — after Task 015
Confirm HomeScreen behavior/tests pass and the coordinator is materially smaller. Confirm no worker
ownership or persistent-format semantics changed.

## Checkpoint B — after Task 020
Confirm EpisodeBrowserScreen is split mechanically and prefetch/playback/download boundaries remain
unchanged.

## Checkpoint C — after Task 027
Confirm JellyfinApi public signatures and endpoint semantics stayed stable.

## Checkpoint D — after Task 031
Confirm DownloadManager synchronization and HLS retry/cancellation semantics are unchanged.

## Checkpoint E — after Task 035
Confirm all original tests still run and test_main was reduced only through same-translation-unit
`.inc` extraction.

## Final checkpoint — Task 036
Review architecture docs against actual source and run a final `make refactor-check`.
