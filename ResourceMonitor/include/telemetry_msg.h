#ifndef TELEMETRY_MSG_H
#define TELEMETRY_MSG_H

#include <stdint.h>
#include "mctm_types.h"

#define TELEMETRY_BRIDGE_CHANNEL "telemetry_bridge"
#define TELEMETRY_MSG_SNAPSHOT   1

typedef struct
{
    int status;
} TelemetryReply;

typedef struct
{
    int type;
    uint64_t timestamp_ns;
    uint64_t cycle;

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

    int have_control;
    uint64_t control_cycle;
    ControlState control_state;
    double control_execution_ms;
    double control_budget_ms;
    int control_deadline_missed;
    int control_budget_overrun;
    double target_speed;
    double throttle_percent;
    double brake_percent;

    int mctm_mode;
    int protect_safety;
    int protect_control;
    int allow_low_criticality;
    double safety_utilization;
    double control_utilization;
} HyperSafeTelemetry;

#endif /* TELEMETRY_MSG_H */
