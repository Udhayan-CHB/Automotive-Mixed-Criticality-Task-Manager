#ifndef INFOTAINMENT_MSG_H
#define INFOTAINMENT_MSG_H

#include <stdint.h>
#include "mctm_messages.h"

#define INFOTAINMENT_MSG_STATUS   100

#define INFOTAINMENT_STATE_NORMAL     0
#define INFOTAINMENT_STATE_DEGRADED   1
#define INFOTAINMENT_STATE_SUSPENDED  2

typedef struct
{
    int type;

    uint64_t cycle;

    int state;

    double execution_time_ms;
    double budget_ms;

    int deadline_missed;
    int budget_overrun;

    uint64_t total_cycles;
    uint64_t degraded_cycles;
    uint64_t suspended_cycles;

} InfotainmentStatusMessage;

typedef union
{
    MCTMDecisionMessage decision;
    InfotainmentStatusMessage infotainment;
} InfotainmentRxMessage;

#endif
