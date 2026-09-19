# Terminal dashboard examples

The seven runtime processes now use the same compact header style. The live values depend on the current run.

## SafetyTask

```text
+--------------------------------------------------------------+
| MCTM SAFETY TASK                                             |
| HIGHEST-criticality periodic safety loop                     |
+--------------------------------------------------------------+
| Priority           : 30 (SCHED_FIFO)                         |
| Period             : 100 ms                                  |
| Budget / deadline  : 20 / 100 ms                             |
| Inputs             : VehicleIO -> ultrasonic / Hall / IR     |
| Outputs            : VehicleIO safety mode + status IPC      |
| Peers              : ControlTask + ResourceMonitor           |
| Fault source       : /mctm_fault_injector                    |
+--------------------------------------------------------------+
```

## ControlTask

```text
+--------------------------------------------------------------+
| MCTM CONTROL TASK                                            |
| HIGH-criticality deterministic control loop                   |
+--------------------------------------------------------------+
| Priority           : 25 (low=10)                             |
| Budget / period    : 10 / 50 ms                              |
| Deadline           : 50 ms                                   |
| Scheduling         : SCHED_SPORADIC                          |
| IPC peer           : resource_monitor                        |
| Trace              : /tmp/schedule_trace.csv                |
| Fault source       : /mctm_fault_injector                    |
+--------------------------------------------------------------+
```

## ResourceMonitor

```text
+--------------------------------------------------------------+
| MCTM RESOURCE MONITOR                                        |
| Mixed-criticality policy engine                               |
+--------------------------------------------------------------+
| Role               : budget + deadline monitor                |
| Priority           : 20 (SCHED_FIFO)                         |
| Policy             : NORMAL -> DEGRADED -> EMERGENCY         |
| IPC                : Safety + Control + Infotainment          |
| Snapshot           : /tmp/mctm_dashboard_status.json         |
| Channel            : resource_monitor                       |
| Startup mode       : DEGRADED (until peers report)           |
+--------------------------------------------------------------+
```

## Infotainment

```text
+--------------------------------------------------------------+
| MCTM INFOTAINMENT TASK                                       |
| LOW-criticality workload                                     |
+--------------------------------------------------------------+
| Priority           : 10                                      |
| Period             : 100 ms                                  |
| Normal workload    : 20 ms                                   |
| Degraded workload  : 5 ms                                    |
| IPC peer           : resource_monitor                        |
| Trace              : /tmp/schedule_trace.csv                |
+--------------------------------------------------------------+
```

## SafetyMonitor

```text
+--------------------------------------------------------------+
| HYPERSAFE SAFETY MONITOR                                     |
| Independent supervisory layer                                 |
+--------------------------------------------------------------+
| Priority           : 15 (SCHED_FIFO)                         |
| Supervisor period  : 100 ms                                  |
| Safety stale       : 300 ms                                  |
| Control stale      : 200 ms                                  |
| RM snapshot stale  : 300 ms                                  |
| IPC source         : resource_monitor                        |
+--------------------------------------------------------------+
```

## VehicleIO

```text
+--------------------------------------------------------------+
| MCTM VEHICLE IO                                              |
| Deterministic software I/O service                            |
+--------------------------------------------------------------+
| Channel            : vehicle_io                              |
| Inputs             : ultrasonic + Hall + IR + brake          |
| Output             : safety mode / display / servo           |
| Hardware           : simulated interface in this build        |
+--------------------------------------------------------------+
```

## FaultInjector

The injector keeps its one-shot command output as a compact state card because it is also used as experiment evidence.

```text
+--------------------------------------------------------------+
| MCTM FAULT INJECTOR                                          |
| Software-only fault control                                   |
+--------------------------------------------------------------+
| Fault              : CONTROL_25MS (8)                        |
| Active             : YES                                     |
| Workload           : 25 ms                                   |
| Delay              : 0 ms                                    |
| Drop status        : NO                                      |
| Stale status       : NO                                      |
| Sensor fault       : NO                                      |
+--------------------------------------------------------------+
```
