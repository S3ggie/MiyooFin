# Task 38 — Receive Jellyfin LibraryChanged events

**Phase:** E — incremental synchronization

## Objective

Add a bounded, cancellable background transport for Jellyfin’s
`LibraryChanged` WebSocket messages, including reconnect behavior and event
coalescing without changing catalog ownership.

## Why

Jellyfin reports added, removed, and updated item IDs in real time.  These
events provide the low-latency path while the restart catch-up and periodic
authoritative reconciliation handle events missed while disconnected.

## Preconditions

- Tasks 35–37 pass and are committed.
- The current authenticated Session and route policy are reused.
- The exact Jellyfin WebSocket protocol supported by the target server is
  verified before implementation.

## Allowed Files

- Create: `src/net/JellyfinLibraryEvents.*`
- `src/net/JellyfinApi.*` only for narrow event-message parsing/declarations
- `src/net/RouteRequest.*` only if existing route selection must be reused
  without changing route policy
- `src/library/LibrarySync.*` only for lifecycle handoff and bounded event
  queue ownership
- `tests/cases/test_api_session.inc`
- `tests/cases/test_misc_regressions.inc`
- This task file

## Forbidden Scope

- Do not perform SQLite writes in the transport.
- Do not create detached threads or an unbounded event queue.
- Do not treat WebSocket connection success as catalog synchronization
  success.
- Do not assume events can be replayed after downtime.
- Do not change artwork, Home, playback, download, or schema behavior.

## Required behavior

1. Parse `LibraryChanged` added, removed, and updated ID lists.
2. Coalesce duplicate IDs with bounded memory while retaining removal/update
   information needed by the consumer.
3. Reconnect after transient disconnects with bounded backoff and
   cancellation-aware shutdown.
4. Never log access tokens or authenticated URLs.
5. Report connection loss as “catch-up required,” not as destructive catalog
   failure.
6. Keep all socket/network work off the SDL/UI thread.

## Method

1. Add parser tests for valid, empty, duplicate, partial, unknown-message,
   malformed, and oversized messages.
2. Add lifecycle tests for cancellation, reconnect, bounded backoff, and
   queue overflow/coalescing.
3. Verify failures before implementing the transport.
4. Implement the smallest transport compatible with the server protocol and
   existing worker lifecycle.

## Validation

```sh
make output/test/test_runner -j2 && output/test/test_runner
make test -j2
make -j2
make onionos
make verify-arm
make refactor-check
git diff --check
```

## STOP conditions

- The target Jellyfin/server/library combination does not support the
  required event protocol.
- The implementation requires a new unbounded worker, detached thread,
  second SQLite connection, or UI-thread network operation.
- Correctness requires inventing replay semantics Jellyfin does not provide.
- Required production work falls outside **Allowed Files**.

## Commit boundary

```text
feat(sync): receive jellyfin library change events
```

Do not begin Task 39 until this task is committed.
