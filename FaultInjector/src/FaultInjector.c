/*
 * SPDX-License-Identifier: MIT
 *
 * HyperSafe - Software Fault Injector
 *
 * Usage:
 *   FaultInjector clear
 *   FaultInjector cpu_overload
 *   FaultInjector control_delay
 *   FaultInjector control_deadline
 *   FaultInjector stale_status
 *   FaultInjector missing_status
 *   FaultInjector sensor_invalid
 *   FaultInjector infotainment_load
 *
 * Faults are persistent until "clear" is issued.
 * No physical GPIO/button/actuator is touched.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include "mctm_dashboard.h"

#include "fault_injector.h"

static int g_shm_fd = -1;
static FaultInjectionState *g_state = NULL;


/* === Fault text === */

static const char *fault_text(int type)
{
    switch (type)
    {
        case FAULT_CLEAR:
            return "CLEAR";

        case FAULT_CPU_OVERLOAD:
            return "CPU_OVERLOAD";

        case FAULT_CONTROL_DELAY:
            return "CONTROL_DELAY";

        case FAULT_CONTROL_DEADLINE:
            return "CONTROL_DEADLINE";

        case FAULT_STALE_STATUS:
            return "STALE_STATUS";

        case FAULT_MISSING_STATUS:
            return "MISSING_STATUS";

        case FAULT_SENSOR_INVALID:
            return "SENSOR_INVALID";

        case FAULT_INFOTAINMENT_LOAD:
            return "INFOTAINMENT_LOAD";
        case FAULT_CONTROL_25MS:
        	return "FAULT_CONTROL_25MS";


        default:
            return "UNKNOWN";
    }
}


/* === Parse fault argument === */

static int parse_fault(const char *s)
{
    char *end = NULL;
    long n;

    if (s == NULL)
        return -1;

    n = strtol(s, &end, 10);

    if (end != NULL && *end == '\0')
        return (int)n;

    if (strcmp(s, "clear") == 0)
        return FAULT_CLEAR;

    if (strcmp(s, "cpu_overload") == 0)
        return FAULT_CPU_OVERLOAD;

    if (strcmp(s, "control_delay") == 0)
        return FAULT_CONTROL_DELAY;

    if (strcmp(s, "control_deadline") == 0)
        return FAULT_CONTROL_DEADLINE;

    if (strcmp(s, "stale_status") == 0)
        return FAULT_STALE_STATUS;

    if (strcmp(s, "missing_status") == 0)
        return FAULT_MISSING_STATUS;

    if (strcmp(s, "sensor_invalid") == 0)
        return FAULT_SENSOR_INVALID;

    if (strcmp(s, "infotainment_load") == 0)
        return FAULT_INFOTAINMENT_LOAD;
    if (strcmp(s,"control_25ms")==0)
    	return FAULT_CONTROL_25MS;

    return -1;
}


/* === Shared-memory setup === */

static int open_shared_memory(void)
{
    g_shm_fd =
        shm_open(
            FAULT_SHM_NAME,
            O_CREAT | O_RDWR,
            0666
        );

    if (g_shm_fd == -1)
    {
        perror("[FAULT-INJECTOR] shm_open");
        return -1;
    }

    if (
        ftruncate(
            g_shm_fd,
            sizeof(FaultInjectionState)
        ) == -1
    )
    {
        perror("[FAULT-INJECTOR] ftruncate");

        close(g_shm_fd);
        g_shm_fd = -1;

        return -1;
    }

    g_state =
        mmap(
            NULL,
            sizeof(FaultInjectionState),
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            g_shm_fd,
            0
        );

    if (g_state == MAP_FAILED)
    {
        perror("[FAULT-INJECTOR] mmap");

        g_state = NULL;

        close(g_shm_fd);
        g_shm_fd = -1;

        return -1;
    }

    return 0;
}


/*
 * ---------------------------------------------------------
 * Publish fault configuration
 * ---------------------------------------------------------
 *
 * sequence protocol:
 *
 *   odd  -> writer is updating
 *   even -> snapshot is stable
 *
 * Consumers can therefore reject a partially-written state.
 */

static void publish(FaultInjectionState *next)
{
    uint32_t seq;

    seq = g_state->sequence;

    if ((seq & 1U) == 0U)
        seq++;
    else
        seq += 2U;

    /* === Mark update as in-progress. === */
    g_state->sequence = seq;

    /* === Copy payload. === */
    g_state->fault_type =
        next->fault_type;

    g_state->active =
        next->active;

    g_state->workload_ms =
        next->workload_ms;

    g_state->delay_ms =
        next->delay_ms;

    g_state->suppress_status =
        next->suppress_status;

    g_state->stale_status =
        next->stale_status;

    g_state->synthetic_sensor_invalid =
        next->synthetic_sensor_invalid;

    g_state->activation_count =
        next->activation_count;

    /* === Even sequence = stable snapshot. === */
    g_state->sequence =
        seq + 1U;
}


/* === Print active state === */

static void print_state(const FaultInjectionState *v)
{
    char value[40];

    mctm_dash_header("MCTM FAULT INJECTOR", "Software-only fault control");

    snprintf(value, sizeof(value), "%s (%d)",
             fault_text(v->fault_type), v->fault_type);
    mctm_dash_row("Fault", value);
    mctm_dash_row("Active", v->active ? "YES" : "NO");

    snprintf(value, sizeof(value), "%u ms", v->workload_ms);
    mctm_dash_row("Workload", value);

    snprintf(value, sizeof(value), "%u ms", v->delay_ms);
    mctm_dash_row("Delay", value);

    mctm_dash_row("Drop status", v->suppress_status ? "YES" : "NO");
    mctm_dash_row("Stale status", v->stale_status ? "YES" : "NO");
    mctm_dash_row("Sensor fault", v->synthetic_sensor_invalid ? "YES" : "NO");

    snprintf(value, sizeof(value), "%llu",
             (unsigned long long)v->activation_count);
    mctm_dash_row("Activation", value);

    snprintf(value, sizeof(value), "%u", v->sequence);
    mctm_dash_row("Sequence", value);
    mctm_dash_footer();
}


/* === Configure fault === */

static void configure_fault(
        int fault,
        FaultInjectionState *state)
{
    memset(
        state,
        0,
        sizeof(*state)
    );

    state->fault_type = fault;

    switch (fault)
    {
        /* === CLEAR === */
        case FAULT_CLEAR:

            state->active = 0;

            break;


        /*
         * -------------------------------------------------
         * CPU OVERLOAD
         * -------------------------------------------------
         *
         * ControlTask runs for approximately 12 ms.
         */
        case FAULT_CPU_OVERLOAD:

            state->active = 1;
            state->workload_ms = 12;

            break;


        /*
         * -------------------------------------------------
         * CONTROL DELAY
         * -------------------------------------------------
         *
         * Normal workload + 20 ms artificial delay.
         *
         * Expected:
         *     execution ≈ 25 ms
         *     budget = 10 ms
         *     deadline = 50 ms
         *
         * Therefore:
         *     budget overrun
         *     no deadline miss
         */
        case FAULT_CONTROL_DELAY:

            state->active = 1;
            state->delay_ms = 20;

            break;


        /*
         * -------------------------------------------------
         * CONTROL DEADLINE
         * -------------------------------------------------
         *
         * Explicit 60 ms CPU workload.
         *
         * Expected:
         *     workload > 50 ms deadline
         *     workload > 10 ms budget
         */
        case FAULT_CONTROL_DEADLINE:

            state->active = 1;
            state->workload_ms = 60;

            break;


        /*
         * -------------------------------------------------
         * STALE STATUS
         * -------------------------------------------------
         *
         * The task must continue publishing status,
         * but the status cycle must stop advancing.
         *
         * suppress_status remains 0.
         */
        case FAULT_STALE_STATUS:

            state->active = 1;
            state->stale_status = 1;

            break;


        /*
         * -------------------------------------------------
         * MISSING STATUS
         * -------------------------------------------------
         *
         * The task completely stops publishing status.
         */
        case FAULT_MISSING_STATUS:

            state->active = 1;
            state->suppress_status = 1;

            break;


        /*
         * -------------------------------------------------
         * SENSOR INVALID
         * -------------------------------------------------
         *
         * Software-only synthetic sensor corruption.
         */
        case FAULT_SENSOR_INVALID:

            state->active = 1;
            state->synthetic_sensor_invalid = 1;

            break;


        /*
         * -------------------------------------------------
         * INFOTAINMENT LOAD
         * -------------------------------------------------
         *
         * 20 ms low-criticality workload.
         */
        case FAULT_INFOTAINMENT_LOAD:

            state->active = 1;
            state->workload_ms = 20;

            break;
        case FAULT_CONTROL_25MS:
        	state->active =1;
        	state->workload_ms=25;
        	break;


        default:

            state->active = 0;

            break;
    }
}


/* === MAIN === */

int main(
        int argc,
        char *argv[])
{
    FaultInjectionState next;
    int fault;

    if (argc != 2)
    {
        printf(
            "Usage:\n"
            "  FaultInjector clear\n"
            "  FaultInjector cpu_overload\n"
            "  FaultInjector control_delay\n"
            "  FaultInjector control_deadline\n"
            "  FaultInjector stale_status\n"
            "  FaultInjector missing_status\n"
            "  FaultInjector sensor_invalid\n"
            "  FaultInjector infotainment_load\n"
        	"  FaultInjector control_25ms"
        );

        return EXIT_FAILURE;
    }

    fault =
        parse_fault(argv[1]);

    if (
        fault < FAULT_CLEAR ||
		fault > FAULT_CONTROL_25MS
    )
    {
        printf(
            "[FAULT-INJECTOR] Invalid fault: %s\n",
            argv[1]
        );

        return EXIT_FAILURE;
    }

    if (open_shared_memory() != 0)
        return EXIT_FAILURE;

    configure_fault(
        fault,
        &next
    );

    /* === Preserve and increment the activation counter. === */
    next.activation_count =
        g_state->activation_count + 1ULL;

    publish(&next);

    /* === Read back the stable sequence for display. === */
    next.sequence =
        g_state->sequence;

    print_state(&next);

    munmap(
        g_state,
        sizeof(FaultInjectionState)
    );

    close(g_shm_fd);

    return EXIT_SUCCESS;
}
