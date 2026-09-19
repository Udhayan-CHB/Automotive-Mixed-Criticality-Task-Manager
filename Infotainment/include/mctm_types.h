#ifndef MCTM_TYPES_H
#define MCTM_TYPES_H

#include <stdint.h>

/* === CRITICALITY LEVELS === */

#define CRITICALITY_LOW       1
#define CRITICALITY_HIGH      2
#define CRITICALITY_HIGHEST   3


/* === SAFETY STATE === */

typedef enum
{
    SAFETY_NORMAL = 0,
    SAFETY_WARNING,
    SAFETY_CRITICAL

} SafetyState;


/* === SAFETY REASON === */

typedef enum
{
    REASON_NONE = 0,

    REASON_HIGH_TEMPERATURE,
    REASON_COLLISION_RISK,
    REASON_BRAKE_ANOMALY,
    REASON_CRITICAL_BRAKING

} SafetyReason;


/* =========================================================
 * VEHICLE STATE
 *
 * Kept for protocol compatibility.
 * Current physical system does not require all fields.
 * ========================================================= */

typedef struct
{
    double temperature;
    double brake_pressure;
    double obstacle_distance;
    double vehicle_speed;

    int brake_pressed;

} VehicleState;


/* === CONTROL STATE === */

typedef enum
{
    CONTROL_NORMAL = 0,
    CONTROL_CAUTION,
    CONTROL_EMERGENCY

} ControlState;


/* =========================================================
 * RESOURCE MONITOR SNAPSHOT
 *
 * Internal structure used by ResourceMonitor.
 * Keeping it here allows all processes to compile against
 * the same shared type definition.
 * ========================================================= */

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


/* === MCTM OPERATING MODES === */

#define MCTM_MODE_NORMAL       0
#define MCTM_MODE_DEGRADED     1
#define MCTM_MODE_EMERGENCY    2

#endif /* MCTM_TYPES_H */
