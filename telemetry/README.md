# MiyooFin Performance Telemetry Roadmap v2

Complete replacement roadmap, re-audited against `main` @ `ef79ba85229f22d270fcf57072823050c371bf33`.

There are **39 tasks (000–038)**. `SCHEMA_V1.md` is normative before Task 000.

Core architecture:

```text
instrumented code
    ↓
PerformanceTelemetry facade
    ↓
fixed bounded producer ring
    ↓
telemetry service thread
    ├── LinuxProcessMetrics
    └── TelemetryWriter
            ↓
        .mft trace
            ↓
        laptop tools
```

`UiDiagnostics` remains the UI watchdog.

## Three execution modes

1. `PERF_TELEMETRY=0`: compile-out. Telemetry production `.cpp` files are not linked and hot-path telemetry code is compile-time eliminated.
2. `PERF_TELEMETRY=1`, runtime off: default. No telemetry thread/file; hot paths check a cheap enabled flag before timing/context work.
3. `PERF_TELEMETRY=1`, `MIYOOFIN_TELEMETRY=1`: active trace.

Current MiyooFin uses the same `output/build`, `output/test`, and `output/build-arm` paths for both compile variants. **Run `make clean` before every switch between `PERF_TELEMETRY=0` and `1`.**

Read: `EXECUTION_RULES.md`, `ARCHITECTURE.md`, `SCHEMA_V1.md`, `SECURITY.md`, then one task.
