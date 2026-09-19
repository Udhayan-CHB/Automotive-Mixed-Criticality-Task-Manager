#ifndef RESOURCE_MONITOR_H
#define RESOURCE_MONITOR_H

#include "mctm_messages.h"
#include "safety_monitor_msg.h"

#define MCTM_MODE_NORMAL      0
#define MCTM_MODE_DEGRADED    1
#define MCTM_MODE_EMERGENCY   2

#define RM_WARNING_UTILIZATION   0.70
#define RM_CRITICAL_UTILIZATION  0.90

typedef struct
{
    double safety_utilization;
    double control_utilization;

    int safety_deadline_missed;
    int safety_budget_overrun;

    int control_deadline_missed;
    int control_budget_overrun;

    SafetyState safety_state;
    ControlState control_state;
} ResourceSnapshot;

void resource_monitor_evaluate(
    const ResourceSnapshot *snapshot,
    MCTMDecisionMessage *decision
);

#endif
