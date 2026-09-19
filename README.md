# Automotive Mixed-Criticality Task Manager (MCTM)

A QNX 8.0 implementation of a mixed-criticality workload manager for a Raspberry Pi target. The project keeps safety-critical and control workloads ahead of low-criticality infotainment work, monitors execution budget/deadline behavior, and changes the system policy when resource pressure is detected.

## Architecture

- **SafetyTask** — highest criticality; periodic safety evaluation and safety-state publication.
- **ControlTask** — high criticality; deterministic control workload with QNX sporadic scheduling, execution-budget and deadline checks.
- **ResourceMonitor** — MCTM decision engine; evaluates task utilization/status and selects NORMAL, DEGRADED, or EMERGENCY policy.
- **Infotainment** — low criticality; workload is reduced or suppressed as MCTM pressure increases.
- **SafetyMonitor** — independent supervisory layer for stale status, regressions, and timing inconsistencies.
- **VehicleIO** — QNX IPC service for vehicle-facing inputs/outputs. The checked-in implementation is a deterministic simulator; physical GPIO/CAN integration can be added behind the same message boundary.
- **FaultInjector** — software fault source used for repeatable overload, deadline, delay, stale-status, and sensor-fault tests.
- **TraceViewer** — CLI parser for `/tmp/schedule_trace.csv`.

## Key scheduling policy

| Workload       | Criticality | Priority | Period       | Budget | Deadline |
|----------------|-------------|----------|--------------|--------|----------|
| SafetyTask     | Highest     | 30       | 100 ms       | 20 ms  | 100 ms   |
| ControlTask    | High        | 25       | 50 ms        | 10 ms  | 50 ms    |
| ResourceMonitor| Monitor     | 20       | event-driven | -      |  -       |
| Infotainment   | Low         | 10       | 100 ms       | 30 ms  | 100 ms   |

The control workload uses `SCHED_SPORADIC`. The MCTM policy is deliberately conservative at startup and moves through `NORMAL -> DEGRADED -> EMERGENCY` as pressure increases.

## Repository layout

```text
MCTM_final_repo/
├── common/include/mctm_dashboard.h
├── ControlTask/
├── FaultInjector/
├── Infotainment/
├── ResourceMonitor/
├── SafetyMonitor/
├── SafetyTask/
├── TraceViewer/
├── VehicleIO/
├── scripts/build_all.sh
├── scripts/run_order.txt
├── .gitignore
└── README.md
```

Generated build directories and Momentics project metadata are intentionally not tracked. Each component has a small Makefile so the source tree remains usable outside the IDE.

## Build on QNX

From the repository root:

```sh
./scripts/build_all.sh
```

Or build a single component:

```sh
make -C ControlTask
make -C ResourceMonitor
```

The Makefiles default to `aarch64le-debug`; override `PLATFORM` or `BUILD_PROFILE` when needed.

## Run the seven runtime terminals

Follow [`scripts/run_order.txt`](scripts/run_order.txt). The fault injector is a one-shot utility, so it is run in the seventh terminal only when a fault scenario is being tested.

## Fault injection examples

```sh
./FaultInjector/build/aarch64le-debug/FaultInjector clear
./FaultInjector/build/aarch64le-debug/FaultInjector cpu_overload
./FaultInjector/build/aarch64le-debug/FaultInjector control_deadline
./FaultInjector/build/aarch64le-debug/FaultInjector control_25ms
./FaultInjector/build/aarch64le-debug/FaultInjector missing_status
```

Faults are written to the shared-memory object `/mctm_fault_injector` and are cleared explicitly with `clear`.

## Evidence collected for the project

The runtime produces two useful forms of evidence:

1. **QNX System Profiler `.kev` traces** for scheduling/activity analysis.
2. **`/tmp/schedule_trace.csv`** for the CLI `TraceViewer` summary.

The final report/presentation should pair the trace with the terminal output that shows the MCTM mode change and the injected fault.

## Notes

- The code uses QNX Neutrino IPC (`name_attach`, `name_open`, `MsgSend`, `MsgReceive`, `MsgReply`) rather than sockets for the task-to-task control path.
- The current VehicleIO process is a deterministic software simulator. It deliberately exposes the same QNX message contract that SafetyTask expects, which keeps hardware access isolated from task logic.
- `telemetry_msg.h` defines the optional ResourceMonitor telemetry protocol referenced by the current code; a TelemetryBridge application is not required for normal MCTM operation.

## License

MIT. Source files carry the SPDX identifier where appropriate.
