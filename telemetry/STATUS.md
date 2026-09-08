# Telemetry Roadmap Status

Current main baseline used for this roadmap: `ef79ba85229f22d270fcf57072823050c371bf33`.

There are **39 tasks, 000–038**.

| Task | Title | Dependencies | Risk | Status |
|---:|---|---|---|---|
| 000 | establish compile contract and schema-stable IDs | — | Medium | NOT STARTED |
| 001 | add monotonic telemetry clocks | 000 | Low | NOT STARTED |
| 002 | add runtime config and no-op facade | 001 | Medium | NOT STARTED |
| 003 | create early enum-only context and hot-path timing guards | 002 | Medium | NOT STARTED |
| 004 | implement the bounded ARM-safe MPSC ring | 003 | High | NOT STARTED |
| 005 | implement exact MFT v1 codec | 004 | High | NOT STARTED |
| 006 | add buffered trace writer | 005 | High | NOT STARTED |
| 007 | connect ring writer service thread rotation and core health | 006 | High | NOT STARTED |
| 008 | integrate telemetry into the real process lifecycle | 007 | High | NOT STARTED |
| 009 | add desktop decoder and golden traces | 008 | Low | NOT STARTED |
| 010 | implement Linux process metrics | 009 | High | NOT STARTED |
| 011 | emit periodic SystemSample records | 010 | High | NOT STARTED |
| 012 | establish periodic Worker Artwork Download and Health aggregates | 011 | High | NOT STARTED |
| 013 | enforce low-storage trace shutdown | 012 | High | NOT STARTED |
| 014 | add in-memory frame timing aggregation | 013 | High | NOT STARTED |
| 015 | instrument App frame boundaries with runtime-off clock guard | 014 | High | NOT STARTED |
| 016 | record safe UI and playback state transitions | 015 | Medium | NOT STARTED |
| 017 | bridge UiDiagnostics with exact phase scope and worker-mask IDs | 016 | High | NOT STARTED |
| 018 | instrument ScreenStack retirement worker | 017 | High | NOT STARTED |
| 019 | instrument Home library sync worker | 018 | High | NOT STARTED |
| 020 | instrument Home hierarchy and poster workers with artwork request context | 019 | High | NOT STARTED |
| 021 | instrument Home decode worker and establish decode context | 020 | High | NOT STARTED |
| 022 | instrument Home lightweight refresh workers | 021 | Medium | NOT STARTED |
| 023 | instrument EpisodeBrowser fetch worker | 022 | High | NOT STARTED |
| 024 | instrument Episode artwork worker and contexts | 023 | High | NOT STARTED |
| 025 | establish MovieDetails artwork context | 024 | Low | NOT STARTED |
| 026 | make ImageCache the sole generic cache metric owner | 025 | Medium | NOT STARTED |
| 027 | make ImageDecoder the sole decode metric owner | 026 | Medium | NOT STARTED |
| 028 | annotate Jellyfin semantic RequestKind context | 027 | High | NOT STARTED |
| 029 | annotate RouteRequest LAN public and fallback attempts | 028 | High | NOT STARTED |
| 030 | make HttpClient the sole normal transport measurement owner | 029 | High | NOT STARTED |
| 031 | feed DownloadManager worker and DownloadSample gauges | 030 | High | NOT STARTED |
| 032 | instrument direct HLS segment attempts retries and throughput | 031 | High | NOT STARTED |
| 033 | instrument playback handoff return and sampler suspension | 032 | High | NOT STARTED |
| 034 | add raw MFT secret-leak regression | 033 | Medium | NOT STARTED |
| 035 | add laptop analysis correlation and exports | 034 | Medium | NOT STARTED |
| 036 | add repository benchmark workflow and comparison tool | 035 | Low | NOT STARTED |
| 037 | run real Miyoo A B C and low-storage validation | 036 | High | NOT STARTED |
| 038 | freeze MFT v1 and finalize architecture documentation | 037 | Low | NOT STARTED |

## Mandatory review gates

- after 008 — core transport + real process lifecycle
- after 013 — Linux sampler + periodic aggregate cadence + low-storage safety
- after 018 — frame/UI/watchdog/retirement
- after 030 — artwork/network stack
- after 032 — DownloadManager/HLS
- after 033 — playback
- after 037 — real-device observer effect
- after 038 — final schema/integration
