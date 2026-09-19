/* SPDX-License-Identifier: MIT */
#ifndef VEHICLE_IO_MSG_H
#define VEHICLE_IO_MSG_H

#include <stdint.h>

#define VIO_MSG_READ_INPUTS   1
#define VIO_MSG_SET_MODE      2
#define VIO_MSG_SET_DISPLAY   3
#define VIO_MSG_SET_SERVO     4

#define VIO_SAFETY_NORMAL     0
#define VIO_SAFETY_WARNING    1
#define VIO_SAFETY_CRITICAL   2

typedef struct {
    uint8_t  obstacle_ir;
    uint8_t  brake_pressed;
    uint8_t  fault_request;
    uint8_t  ultrasonic_valid;
    uint8_t  hall_valid;
    uint8_t  reserved[3];
    float    obstacle_distance_cm;
    float    wheel_rpm;
    uint32_t hall_pulses;
} VehicleIoInputs;

typedef struct {
    int      type;
    int      mode;
    double   temperature_c;
    int      temp_valid;
    uint32_t display_value;
    int      servo_us;
} VehicleIoMsg;

typedef struct {
    int             status;
    VehicleIoInputs inputs;
} VehicleIoReply;

#endif
