/* SPDX-License-Identifier: MIT
 *
 * VehicleIO provides the QNX IPC boundary for vehicle-facing inputs and
 * safety outputs. The current implementation is a deterministic simulator;
 * it keeps the rest of the MCTM stack independent from physical GPIO while
 * the interface is being integrated with the target hardware.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>

#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include "mctm_dashboard.h"

#include "vehicle_io_msg.h"

#define VEHICLE_IO_CHANNEL "vehicle_io"

static void fill_inputs(VehicleIoInputs *inputs, uint32_t cycle)
{
    memset(inputs, 0, sizeof(*inputs));

    /* Keep the default vehicle state safe and boring for repeatable tests. */
    inputs->ultrasonic_valid = 1;
    inputs->hall_valid = 1;
    inputs->obstacle_ir = 0;
    inputs->brake_pressed = 0;
    inputs->fault_request = 0;
    inputs->obstacle_distance_cm = 150.0f;
    inputs->wheel_rpm = 1200.0f + (float)(cycle % 20U);
    inputs->hall_pulses = cycle * 2U;
}

int main(void)
{
    name_attach_t *attach;
    VehicleIoInputs inputs;
    int safety_mode = VIO_SAFETY_NORMAL;
    uint32_t cycle = 0;

    mctm_dash_header("MCTM VEHICLE IO", "Deterministic software I/O service");
    mctm_dash_row("Channel", VEHICLE_IO_CHANNEL);
    mctm_dash_row("Inputs", "ultrasonic + Hall + IR + brake");
    mctm_dash_row("Output", "safety mode / display / servo");
    mctm_dash_row("Hardware", "simulated interface in this build");
    mctm_dash_footer();

    attach = name_attach(NULL, VEHICLE_IO_CHANNEL, 0);
    if (attach == NULL)
    {
        fprintf(stderr, "[VEHICLE-IO] name_attach(%s) failed: %s\n",
                VEHICLE_IO_CHANNEL, strerror(errno));
        return EXIT_FAILURE;
    }

    printf("[VEHICLE-IO] ready | mode=NORMAL | distance=150.0 cm | rpm=1200\n");
    fflush(stdout);

    for (;;)
    {
        VehicleIoMsg msg;
        VehicleIoReply reply;
        int rcvid;

        memset(&msg, 0, sizeof(msg));
        memset(&reply, 0, sizeof(reply));

        rcvid = MsgReceive(attach->chid, &msg, sizeof(msg), NULL);
        if (rcvid == -1)
        {
            if (errno == EINTR)
                continue;
            perror("[VEHICLE-IO] MsgReceive");
            break;
        }

        if (rcvid == 0)
            continue;

        fill_inputs(&inputs, cycle++);
        reply.status = 0;
        reply.inputs = inputs;

        switch (msg.type)
        {
            case VIO_MSG_READ_INPUTS:
                break;

            case VIO_MSG_SET_MODE:
                if (msg.mode < VIO_SAFETY_NORMAL ||
                    msg.mode > VIO_SAFETY_CRITICAL)
                {
                    reply.status = EINVAL;
                    break;
                }

                if (safety_mode != msg.mode)
                {
                    safety_mode = msg.mode;
                    printf("[VEHICLE-IO] mode changed -> %s\n",
                           safety_mode == VIO_SAFETY_NORMAL ? "NORMAL" :
                           safety_mode == VIO_SAFETY_WARNING ? "WARNING" : "CRITICAL");
                    fflush(stdout);
                }
                break;

            case VIO_MSG_SET_DISPLAY:
            case VIO_MSG_SET_SERVO:
                /* The simulator accepts these commands but has no actuator. */
                break;

            default:
                reply.status = ENOSYS;
                break;
        }

        if (MsgReply(rcvid, EOK, &reply, sizeof(reply)) == -1)
        {
            fprintf(stderr, "[VEHICLE-IO] MsgReply failed: %s\n",
                    strerror(errno));
        }
    }

    name_detach(attach, 0);
    return EXIT_SUCCESS;
}
