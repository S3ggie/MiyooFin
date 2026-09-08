# Performance telemetry benchmark result

Status: **COMPLETE with soak waived by user decision**

The required short-duration real-device scenarios and controlled low-storage
check were completed on a Miyoo Mini Plus. The requested 1–2 hour active soak
was intentionally not performed; it is recorded as **NOT PERFORMED**, not as a
successful soak.

## Environment

| Field | Value |
|---|---|
| Date | 2026-09-08 |
| Device | Miyoo Mini Plus at `192.168.1.197` |
| OnionOS / firmware | Existing device image; exact versions not captured |
| Storage | `/mnt/SDCARD`; approximately 51.3–51.5 GiB free during normal runs |
| Network/server/media | Same configured Wi-Fi, Jellyfin server, and representative media throughout |
| Application condition | Normal Onion launcher; no direct `miyoofin` launch |
| A/B build evidence | Clean ARM builds and deployment completed at the prior valid benchmark revision `902b392` |
| C revalidation | Clean telemetry-enabled package and remaining playback validation at `87c5f91` |
| Output filesystem | `/mnt/SDCARD/App/MiyooFin/telemetry-logs` |

## A/B/C result

| Variant | Result | Evidence |
|---|---|---|
| A — `PERF_TELEMETRY=0` | PASS | Clean compile-out build, ARM verification, package deployment, and normal `./launch.sh` launch completed. |
| B — compiled in, `MIYOOFIN_TELEMETRY=0` | PASS | The B/C benchmark pair used the same compiled-in binary; runtime-off launch produced no telemetry thread or trace file. |
| C — compiled in, `MIYOOFIN_TELEMETRY=1` | PASS | Normal launcher created valid `MFT1` traces on the SD-card filesystem; `TelemetryStarted` decoded successfully. Final playback revalidation used the corrected package at `87c5f91`. |

No observer-effect regression was reported during the controlled short
scenarios. Exact A/B/C CPU/RSS/frame-delta medians were not captured, so those
numeric targets remain unmeasured rather than being inferred as passes.

## Scenario evidence

| Scenario | Result | Evidence / notes |
|---|---|---|
| Startup | PASS | User-observed cold startup approximately 5.52 s and warm startup approximately 4.73 s; no regression reported. Additional observation was approximately 4.2 s average. |
| Idle Home, 5 min | PASS | Completed on C. |
| Warm startup | PASS | Completed on C without functional difference from baseline. |
| Controlled cold MiyooFin cache startup | PASS | Completed without Linux `drop_caches`. |
| Aggressive Home navigation | PASS | Completed on C. |
| Episode artwork stress | PASS | Completed on C; artwork remained functional. |
| Library sync | PASS | Completed on C; sync behavior remained functional. |
| Representative download | PASS | C trace contained 378 `DownloadSample` and 228 `DownloadSegmentAttempt` records; successful HTTP 200/CURL 0 segment attempts were decoded. |
| Network playback | PASS | Final C trace contained one five-stage lifecycle, normal child exit, audible playback, and `source=Jellyfin` for all five events. |
| Local/downloaded playback | PASS | Final C trace contained one five-stage lifecycle, normal child exit, and `source=Local` for all five events. |
| Low-storage crossing | PASS | Isolated 132 MiB tmpfs overlay was used; 8 MiB was written there, not to the main SD card. `WriterDisabledLowSpace` decoded with the 128 MiB threshold; final health showed zero drops/errors, telemetry stopped, and the trace did not grow afterward. |
| Active soak, 1–2 hours | NOT PERFORMED | Explicitly waived by the user; no soak result is claimed. |

## Trace evidence

The primary C session trace covered approximately 9.1 minutes and decoded to
14,113 records with zero dropped records and zero writer errors. Its ordinary
trace rate was approximately 1.5 KiB/s. The final playback revalidation traces
also decoded as valid MFT v1 files:

| Trace | Records | Playback lifecycle | Source |
|---|---:|---|---|
| Final network revalidation | 1,225 | RequestToFinalPresent → SuspendPlatform → ChildWait → ResumePlatform → ReturnToFirstNormalFrame | `Jellyfin`; child `Exited`, code `0`; audible audio confirmed |
| Final local revalidation | 11,937 | RequestToFinalPresent → SuspendPlatform → ChildWait → ResumePlatform → ReturnToFirstNormalFrame | `Local`; child `Exited`, code `0` |

The selected network MPEG-TS stream was independently confirmed to contain
H.264 video and AAC stereo audio. The final package used the Onion DSP audio
bridge, and the final network playback was audible.

## Low-storage and rotation results

| Check | Result | Evidence |
|---|---|---|
| Startup below cutoff creates no trace | PASS via automated validation | Existing low-storage startup tests passed. |
| Runtime crossing below cutoff disables tracing | PASS | `WriterDisabledLowSpace` decoded from the isolated tmpfs run. |
| Final health and writer close | PASS | Zero drops/errors; telemetry service stopped and no further trace growth was observed. |
| Main SD-card safety | PASS | No tens-of-gigabytes allocation; temporary filler was confined to the mounted 132 MiB overlay and removed. |
| 16 MiB rotation / four-file retention | NOT RUN | Not performed; no rotation claim is made. |

## Privacy review

| Check | Result | Evidence |
|---|---|---|
| Raw MFT contains no authenticated URLs or HTTP bodies | PASS | Decoder/privacy checks passed; copied traces were inspected without exposing credentials. |
| Telemetry output remained on SD-card filesystem | PASS | C trace was created and grew under the configured SD-card telemetry directory. |
| Temporary copied traces handled outside the repository | PASS | Retrieval and decoding used temporary laptop paths; no runtime traces are committed. |

## Conclusion

The required acceptance criteria pass for the performed non-soak scenarios. The
observer-effect result is PASS for the available short-duration A/B/C evidence,
with numeric CPU/RSS/frame-delta medians left unmeasured and the long soak
explicitly waived/not performed.
