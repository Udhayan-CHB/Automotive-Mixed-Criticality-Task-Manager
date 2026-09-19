#ifndef FAULT_INJECTOR_H
#define FAULT_INJECTOR_H

#include <stdint.h>

/* Software-only fault injection types shared by FaultInjector and tasks. */
#define FAULT_CLEAR                 0
#define FAULT_CPU_OVERLOAD          1
#define FAULT_CONTROL_DELAY         2
#define FAULT_CONTROL_DEADLINE      3
#define FAULT_STALE_STATUS          4
#define FAULT_MISSING_STATUS        5
#define FAULT_SENSOR_INVALID        6
#define FAULT_INFOTAINMENT_LOAD     7

#define FAULT_SHM_NAME "/mctm_fault_injector"

typedef struct
{
    uint32_t sequence;
    int fault_type;
    int active;
    uint32_t workload_ms;
    uint32_t delay_ms;
    int suppress_status;
    int synthetic_sensor_invalid;
    uint64_t activation_count;
} FaultInjectionState;

#endif
