#ifndef SAFETY_MONITOR_MSG_H
#define SAFETY_MONITOR_MSG_H

#include <stdint.h>
#include "mctm_types.h"

#define SAFETYMON_MSG_SNAPSHOT 200

typedef struct
{
    int type;
    uint64_t timestamp_ns;

    uint64_t safety_cycle;
    uint64_t control_cycle;

    SafetyState safety_state;
    ControlState control_state;

    double safety_execution_ms;
    double control_execution_ms;

    int safety_deadline_missed;
    int safety_budget_overrun;

    int control_deadline_missed;
    int control_budget_overrun;

    int mctm_mode;

    int have_safety;
    int have_control;
} SafetyMonitorSnapshot;

typedef struct
{
    int status;
} SafetyMonitorReply;

#endif
