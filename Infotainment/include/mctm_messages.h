#ifndef MCTM_MESSAGES_H
#define MCTM_MESSAGES_H

#include <stdint.h>
#include "mctm_types.h"


/* === IPC MESSAGE TYPES === */

#define MCTM_MSG_SAFETY_STATUS    1
#define MCTM_MSG_MCTM_DECISION    2
#define MCTM_MSG_CONTROL_STATUS   3

/*
 * Internal request used when a process wants the current
 * MCTM decision from ResourceMonitor.
 */
#define MCTM_MSG_GET_DECISION     4

/*
 * Request used by TelemetryBridge to obtain a consolidated
 * system snapshot from ResourceMonitor.
 */
#define MCTM_MSG_GET_TELEMETRY    5


/* =========================================================
 * SafetyTask -> ResourceMonitor / ControlTask
 *
 * Safety status and timing information.
 * ========================================================= */

typedef struct
{
    int type;

    uint64_t cycle;

    SafetyState safety_state;
    SafetyReason safety_reason;

    double temperature;
    double brake_pressure;
    double obstacle_distance;
    double vehicle_speed;

    int brake_pressed;

    double execution_time_ms;

    int deadline_missed;
    int budget_overrun;

} SafetyStatusMessage;


/* =========================================================
 * ControlTask -> ResourceMonitor
 *
 * Control status and timing information.
 * ========================================================= */

typedef struct
{
    int type;

    uint64_t cycle;

    int criticality;

    ControlState control_state;

    double execution_time_ms;
    double budget_ms;

    int deadline_missed;
    int budget_overrun;

    double target_speed;
    double throttle_percent;
    double brake_percent;

} ControlStatusMessage;


/* =========================================================
 * ResourceMonitor -> ControlTask
 * ResourceMonitor -> SafetyMonitor
 * ResourceMonitor -> InfotainmentTask
 *
 * MCTM DECISION
 * ========================================================= */

typedef struct
{
    int type;

    uint64_t cycle;

    int mctm_mode;

    int protect_safety;
    int protect_control;
    int allow_low_criticality;

    double safety_utilization;
    double control_utilization;

} MCTMDecisionMessage;


/* === RESOURCE MONITOR GET-DECISION REQUEST === */

typedef struct
{
    int type;

} MCTMGetDecisionRequest;


/* =========================================================
 * RESOURCE MONITOR GET-TELEMETRY REQUEST
 *
 * Used by TelemetryBridge to obtain one consolidated
 * snapshot of SafetyTask + ControlTask + MCTM state.
 * ========================================================= */

typedef struct
{
    int type;

} MCTMGetTelemetryRequest;


/* =========================================================
 * RESOURCE MONITOR -> TELEMETRY BRIDGE
 *
 * Consolidated HyperSafe system snapshot.
 * ========================================================= */

typedef struct
{
    int status;

    uint64_t timestamp_ns;

    /* SafetyTask */

    int have_safety;

    uint64_t safety_cycle;

    SafetyState safety_state;
    SafetyReason safety_reason;

    double temperature;
    double brake_pressure;
    double obstacle_distance;
    double vehicle_speed;

    int brake_pressed;

    double safety_execution_ms;

    int safety_deadline_missed;
    int safety_budget_overrun;


    /* ControlTask */

    int have_control;

    uint64_t control_cycle;

    int control_criticality;

    ControlState control_state;

    double control_execution_ms;
    double control_budget_ms;

    int control_deadline_missed;
    int control_budget_overrun;

    double target_speed;
    double throttle_percent;
    double brake_percent;


    /* MCTM */

    uint64_t mctm_cycle;

    int mctm_mode;

    int protect_safety;
    int protect_control;
    int allow_low_criticality;

    double safety_utilization;
    double control_utilization;

} HyperSafeTelemetrySnapshot;


/* === GENERIC IPC REPLY === */

typedef struct
{
    int status;

} MCTMReply;


/* =========================================================
 * RECEIVE UNION
 *
 * Useful for QNX MsgReceive() dispatch.
 * ========================================================= */

typedef union
{
    SafetyStatusMessage        safety;
    ControlStatusMessage       control;
    MCTMDecisionMessage        decision;
    MCTMGetDecisionRequest     request;
    MCTMGetTelemetryRequest    telemetry_request;

} MCTMMessage;


#endif /* MCTM_MESSAGES_H */
