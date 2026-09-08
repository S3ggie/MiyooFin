# Miyoo Mini Plus Benchmark Protocol

Use the same device, SD card/filesystem, OnionOS, firmware, Wi-Fi/AP placement, Jellyfin server/path/media, application settings, and stock/overclock condition. Disable unrelated VNC/scraping/copy/update work.

## Build rule

Object paths are shared across compile variants. Every 0↔1 switch requires `make clean`.

### A — compiled out
```sh
make clean
make PERF_TELEMETRY=0 test -j2
make PERF_TELEMETRY=0 -j2
make PERF_TELEMETRY=0 onionos
make verify-arm
```
Deploy and launch normally:
```sh
./launch.sh
```

### B — compiled in, runtime off
```sh
make clean
make PERF_TELEMETRY=1 test -j2
make PERF_TELEMETRY=1 -j2
make PERF_TELEMETRY=1 onionos
make verify-arm
```
Deploy once. Launch:
```sh
MIYOOFIN_TELEMETRY=0 ./launch.sh
```
Confirm no telemetry thread/file.

### C — same compiled-in binary, runtime on
Do not rebuild after B. Launch through the normal Onion launcher:
```sh
MIYOOFIN_TELEMETRY=1 ./launch.sh
```
Never benchmark active telemetry by invoking `./miyoofin` directly.

## Scenarios

Idle Home (5 min); warm startup; controlled cold MiyooFin cache startup (no Linux `drop_caches`); aggressive Home navigation; EpisodeBrowser artwork stress; library sync; fixed representative download; network playback; local playback; controlled low-storage threshold crossing; 1–2 hour soak.

Use five runs for short scenarios when practical and report median plus spread.

Low-storage test must verify WriterDisabledLowSpace + final health when crossing during a trace, writer close, enabled flag false, no further trace growth, and unaffected app/download behavior.

Targets: B no thread/file; compile-out no telemetry implementation; runtime-off no telemetry clock/context work; no added baseline UI stalls; <=1 percentage point idle one-core CPU when achievable; <=1 MiB RSS; <=5% p95 frame regression; few KiB/s ordinary trace rate; zero ordinary drops; bounded rotation; low-space shutdown works.
