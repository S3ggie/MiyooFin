# Mandatory Human Review Checkpoints

- **After 008 — core transport + process lifecycle.** Review compile/runtime disable paths, early context guards, ring algorithm, writer/service, rotation, `main()` init-failure cleanup, and host/ARM wiring.
- **After 013 — Linux sampler + aggregate cadence + low-storage shutdown.** Review SystemSample plus WorkerSample/ArtworkSummary/DownloadSample/TelemetryHealth reset semantics and 128 MiB cutoff.
- **After 018 — frame/UI/watchdog/retirement.** Review phase boundaries, runtime-off clock guards, state IDs, UiDiagnostics authority, and retirement gauges.
- **After 030 — artwork + semantic networking.** Review no-double-counting ownership, RequestKind, RouteRequest attempts, HttpClient metrics, and zero URL/string serialization.
- **After 032 — DownloadManager/HLS.** Review five-attempt policy, retry/backoff/fallback invariants, and HLS privacy.
- **After 033 — playback.** Review five playback stages, sampling suspension, and preserved SDL timing reset.
- **After 037 — real-device observer effect.** Mandatory human review of A/B/C and low-storage evidence.
- **After 038 — final schema/integration.** Verify encoder, decoder, docs, and benchmark evidence agree.
