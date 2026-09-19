#ifndef MCTM_CONFIG_H
#define MCTM_CONFIG_H


/* === SAFETY TASK === */

#define SAFETY_PERIOD_MS                  100
#define SAFETY_DEADLINE_MS                100
#define SAFETY_BUDGET_MS                  20
#define SAFETY_WORKLOAD_MS                5


/* === CONTROL TASK === */

#define CONTROL_PERIOD_MS                 50
#define CONTROL_DEADLINE_MS               50
#define CONTROL_BUDGET_MS                 10


/* === CONTROL TASK WORKLOAD === */

#define CONTROL_NORMAL_WORKLOAD_MS         5
#define CONTROL_PRESSURE_WORKLOAD_MS       8
#define CONTROL_OVERLOAD_WORKLOAD_MS       12

#define CONTROL_NORMAL_END_CYCLE           50
#define CONTROL_PRESSURE_END_CYCLE        100


/* === RESOURCE MONITOR === */

#define RESOURCE_MONITOR_PERIOD_MS        100


/* === INFOTAINMENT === */

#define INFOTAINMENT_PERIOD_MS            100
#define INFOTAINMENT_NORMAL_WORKLOAD_MS    20
#define INFOTAINMENT_DEGRADED_WORKLOAD_MS   5


/* =========================================================
 * QNX THREAD PRIORITIES
 *
 * Higher value = higher priority.
 *
 * SafetyTask       = 30
 * ControlTask      = 25
 * ResourceMonitor  = 20
 * Infotainment     = 10
 * ========================================================= */

#define SAFETY_TASK_PRIORITY              30
#define CONTROL_TASK_PRIORITY             25
#define RESOURCE_MONITOR_PRIORITY         20
#define LOW_CRITICALITY_PRIORITY         10


/* =========================================================
 * MCTM UTILIZATION THRESHOLDS
 *
 * Internal representation:
 *     0.70 = 70%
 *     0.90 = 90%
 * ========================================================= */

#define RM_WARNING_UTILIZATION            0.70
#define RM_CRITICAL_UTILIZATION           0.90

#define MCTM_UTILIZATION_WARNING_PERCENT  70.0
#define MCTM_UTILIZATION_CRITICAL_PERCENT 90.0


/* === SAFETY DISTANCE THRESHOLDS === */

#define SAFETY_WARNING_DISTANCE_CM       100.0
#define SAFETY_CRITICAL_DISTANCE_CM       30.0


/* === SENSOR FILTERING === */

#define ULTRASONIC_INVALID_LIMIT           3
#define IR_DETECTED_LIMIT                  3


/* === SOFTWARE FAULT INJECTION === */

#define FAULT_CLEAR                        0
#define FAULT_CPU_OVERLOAD                 1
#define FAULT_CONTROL_DELAY                2
#define FAULT_CONTROL_DEADLINE             3
#define FAULT_STALE_STATUS                 4
#define FAULT_MISSING_STATUS               5
#define FAULT_SENSOR_INVALID               6
#define FAULT_INFOTAINMENT_LOAD            7
#define FAULT_CONTROL_25MS                 8


/* === FAULT PARAMETERS === */

#define FAULT_OVERLOAD_WORKLOAD_MS        12
#define FAULT_SEVERE_OVERLOAD_WORKLOAD_MS 25

#define FAULT_DELAY_MS                    20


/* === RECOVERY POLICY === */

#define MCTM_DEGRADED_RECOVERY_CYCLES     3
#define MCTM_EMERGENCY_RECOVERY_CYCLES    5


/* === STATUS TIMEOUTS === */

#define SAFETY_STATUS_TIMEOUT_MS         250
#define CONTROL_STATUS_TIMEOUT_MS        150
#define INFOTAINMENT_STATUS_TIMEOUT_MS   300


/* === SCHEDULING TRACE === */

#define SCHEDULE_TRACE_FILE \
        "/tmp/schedule_trace.csv"


/* === TASK IDENTIFIERS === */

#define TASK_ID_VEHICLE_IO                1
#define TASK_ID_SAFETY                    2
#define TASK_ID_CONTROL                   3
#define TASK_ID_RESOURCE_MONITOR          4
#define TASK_ID_SAFETY_MONITOR            5
#define TASK_ID_FAULT_INJECTOR            6
#define TASK_ID_INFOTAINMENT              7


/* === SYSTEM CONFIGURATION === */

#define MCTM_MAX_TASKS                    7

#define HYPERSAFE_CONFIG_VERSION          1


#endif /* MCTM_CONFIG_H */
