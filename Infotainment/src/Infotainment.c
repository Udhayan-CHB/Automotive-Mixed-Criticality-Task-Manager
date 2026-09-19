/*
 * SPDX-License-Identifier: MIT
 *
 * HyperSafe - InfotainmentTask
 *
 * Low-criticality workload
 *
 * Low-criticality deterministic workload.
 *
 * NORMAL:
 *     20 ms workload
 *
 * DEGRADED:
 *      5 ms workload
 *
 * EMERGENCY:
 *      workload suppressed
 *
 * ResourceMonitor sends MCTMDecisionMessage through the
 * "infotainment" QNX named channel.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include "mctm_dashboard.h"

#include "mctm_types.h"
#include "mctm_messages.h"
#include "mctm_config.h"
#include "infotainment_msg.h"


#define INFOTAINMENT_CHANNEL_NAME "infotainment"
#define RESOURCE_MONITOR_CHANNEL "resource_monitor"

#ifndef SCHEDULE_TRACE_FILE
#define SCHEDULE_TRACE_FILE "/tmp/schedule_trace.csv"
#endif

#ifndef INFOTAINMENT_BUDGET_MS
#define INFOTAINMENT_BUDGET_MS 30
#endif

/* === Scheduling trace (self-contained) === */

static int g_schedule_trace_fd = -1;

/* Forward declaration used by the trace logger. */
static const char *mode_text(int mode);

static const char *trace_event_text(int event)
{
    switch (event)
    {
        case 1: return "START";
        case 2: return "END";
        default: return "UNKNOWN";
    }
}

static int trace_write_all(const char *buf, size_t len)
{
    size_t offset = 0;

    while (offset < len)
    {
        ssize_t n = write(
            g_schedule_trace_fd,
            buf + offset,
            len - offset
        );

        if (n < 0)
        {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (n == 0)
            return -1;

        offset += (size_t)n;
    }

    return 0;
}

static int scheduling_logger_init(void)
{
    struct stat st;
    const char *header =
        "timestamp_ns,cycle,task,event,execution_ms,deadline,budget,mode,criticality\n";

    if (g_schedule_trace_fd != -1)
        return 0;

    g_schedule_trace_fd = open(
        SCHEDULE_TRACE_FILE,
        O_WRONLY | O_CREAT | O_APPEND,
        0666
    );

    if (g_schedule_trace_fd == -1)
        return -1;

    if (fstat(g_schedule_trace_fd, &st) == 0 && st.st_size == 0)
    {
        if (trace_write_all(header, strlen(header)) != 0)
        {
            close(g_schedule_trace_fd);
            g_schedule_trace_fd = -1;
            return -1;
        }
    }

    return 0;
}

static void scheduling_log(
        uint64_t timestamp_ns,
        uint64_t cycle,
        int event,
        double execution_ms,
        const char *deadline,
        const char *budget,
        int mode)
{
    char line[512];
    int len;

    if (g_schedule_trace_fd == -1)
        return;

    len = snprintf(
        line,
        sizeof(line),
        "%llu,%llu,InfotainmentTask,%s,%.3f,%s,%s,%s,LOW\n",
        (unsigned long long)timestamp_ns,
        (unsigned long long)cycle,
        trace_event_text(event),
        execution_ms,
        deadline != NULL ? deadline : "UNKNOWN",
        budget != NULL ? budget : "UNKNOWN",
        mode_text(mode)
    );

    if (len > 0 && (size_t)len < sizeof(line))
        (void)trace_write_all(line, (size_t)len);
}

static void scheduling_logger_close(void)
{
    if (g_schedule_trace_fd != -1)
    {
        close(g_schedule_trace_fd);
        g_schedule_trace_fd = -1;
    }
}


/* === Global MCTM state === */

static volatile int g_mctm_mode = MCTM_MODE_NORMAL;
static volatile int g_allow_low_criticality = 1;

static pthread_mutex_t g_state_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static MCTMDecisionMessage g_decision;


/* === Runtime counters === */

static uint64_t g_cycle = 0;
static uint64_t g_total_cycles = 0;
static uint64_t g_degraded_cycles = 0;
static uint64_t g_suspended_cycles = 0;


/* === Prevent compiler from eliminating workload arithmetic. === */
static volatile uint64_t g_work_sink = 0;


/* === Time === */

static uint64_t monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
        return 0;

    return
        ((uint64_t)ts.tv_sec * 1000ULL) +
        ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
        return 0;

    return
        ((uint64_t)ts.tv_sec * 1000000000ULL) +
        (uint64_t)ts.tv_nsec;
}


/* === Mode text === */

static const char *mode_text(int mode)
{
    switch (mode)
    {
        case MCTM_MODE_NORMAL:
            return "NORMAL";

        case MCTM_MODE_DEGRADED:
            return "DEGRADED";

        case MCTM_MODE_EMERGENCY:
            return "EMERGENCY";

        default:
            return "UNKNOWN";
    }
}


/* === State text === */

static const char *state_text(int state)
{
    switch (state)
    {
        case INFOTAINMENT_STATE_NORMAL:
            return "NORMAL";

        case INFOTAINMENT_STATE_DEGRADED:
            return "DEGRADED";

        case INFOTAINMENT_STATE_SUSPENDED:
            return "SUSPENDED";

        default:
            return "UNKNOWN";
    }
}


/* === Deterministic CPU workload === */

static void run_workload(
        uint32_t workload_ms)
{
    volatile uint64_t x = 0;

    uint64_t start;
    uint64_t now;

    if (workload_ms == 0)
        return;

    start =
        monotonic_ms();

    do
    {
        x += 3;
        x *= 7;
        x ^= 0x5A5A5A5AULL;
        x %= 1000003ULL;

        now =
            monotonic_ms();

    } while (
        (now - start) <
        workload_ms
    );

    g_work_sink = x;
}


/* === Update MCTM decision === */

static void update_decision(
        const MCTMDecisionMessage *decision)
{
    int old_mode;

    if (decision == NULL)
        return;

    pthread_mutex_lock(
        &g_state_mutex
    );

    old_mode =
        g_mctm_mode;

    memcpy(
        &g_decision,
        decision,
        sizeof(g_decision)
    );

    g_mctm_mode =
        decision->mctm_mode;

    g_allow_low_criticality =
        decision->allow_low_criticality;

    pthread_mutex_unlock(
        &g_state_mutex
    );

    if (old_mode != decision->mctm_mode)
    {
        printf(
            "[INFOTAINMENT] MCTM transition: %s -> %s "
            "allow_low=%d "
            "safety_util=%.3f "
            "control_util=%.3f\n",

            mode_text(old_mode),

            mode_text(
                decision->mctm_mode
            ),

            decision->allow_low_criticality,

            decision->safety_utilization,

            decision->control_utilization
        );

        fflush(stdout);
    }
}


/* =========================================================
 * IPC thread
 *
 * ResourceMonitor -> InfotainmentTask
 * ========================================================= */

static void *ipc_thread_func(
        void *arg)
{
    name_attach_t *attach;

    attach =
        (name_attach_t *)arg;

    printf(
        "[INFOTAINMENT-IPC] IPC thread started\n"
    );

    fflush(stdout);

    while (1)
    {
        MCTMDecisionMessage decision;

        MCTMReply reply;

        int rcvid;


        memset(
            &decision,
            0,
            sizeof(decision)
        );


        rcvid =
            MsgReceive(
                attach->chid,
                &decision,
                sizeof(decision),
                NULL
            );


        if (rcvid == -1)
        {
            if (errno == EINTR)
                continue;

            printf(
                "[INFOTAINMENT-IPC] "
                "MsgReceive failed: %s\n",
                strerror(errno)
            );

            continue;
        }


        /* === Ignore pulses. === */
        if (rcvid == 0)
            continue;


        if (
            decision.type ==
            MCTM_MSG_MCTM_DECISION
        )
        {
            update_decision(
                &decision
            );

            reply.status = 0;

            if (
                MsgReply(
                    rcvid,
                    EOK,
                    &reply,
                    sizeof(reply)
                ) == -1
            )
            {
                printf(
                    "[INFOTAINMENT-IPC] "
                    "MsgReply failed: %s\n",
                    strerror(errno)
                );
            }

            continue;
        }


        printf(
            "[INFOTAINMENT-IPC] "
            "Unknown message type=%d\n",
            decision.type
        );

        MsgError(
            rcvid,
            ENOSYS
        );
    }

    return NULL;
}


/* === Initial MCTM decision === */

static int request_initial_decision(
        int resource_coid)
{
    MCTMGetDecisionRequest request;

    MCTMDecisionMessage reply;


    memset(
        &request,
        0,
        sizeof(request)
    );

    memset(
        &reply,
        0,
        sizeof(reply)
    );


    request.type =
        MCTM_MSG_GET_DECISION;


    if (
        MsgSend(
            resource_coid,
            &request,
            sizeof(request),
            &reply,
            sizeof(reply)
        ) == -1
    )
    {
        printf(
            "[INFOTAINMENT] "
            "Initial decision request failed: %s\n",
            strerror(errno)
        );

        return -1;
    }


    if (
        reply.type !=
        MCTM_MSG_MCTM_DECISION
    )
    {
        printf(
            "[INFOTAINMENT] "
            "Invalid decision type=%d\n",
            reply.type
        );

        return -1;
    }


    update_decision(
        &reply
    );


    printf(
        "[INFOTAINMENT] Initial MCTM=%s "
        "allow_low=%d\n",

        mode_text(
            reply.mctm_mode
        ),

        reply.allow_low_criticality
    );

    return 0;
}


/* === Workload thread === */

static void *workload_thread_func(
        void *arg)
{
    (void)arg;

    uint64_t next_release;


    printf(
        "[INFOTAINMENT] Workload thread started\n"
    );

    fflush(stdout);


    next_release =
        monotonic_ms();


    while (1)
    {
        int mode;
        int allow_low;

        int state;

        uint32_t workload_ms;

        uint64_t start_ms;
        uint64_t end_ms;
        uint64_t start_ns;
        uint64_t end_ns;

        double execution_ms;

        int deadline_missed;
        int budget_overrun;


        /* === Read current MCTM state === */

        pthread_mutex_lock(
            &g_state_mutex
        );

        mode =
            g_mctm_mode;

        allow_low =
            g_allow_low_criticality;

        pthread_mutex_unlock(
            &g_state_mutex
        );


        /* === Determine workload === */

        if (
            mode == MCTM_MODE_EMERGENCY
            ||
            !allow_low
        )
        {
            state =
                INFOTAINMENT_STATE_SUSPENDED;

            workload_ms =
                0;

            g_suspended_cycles++;
        }
        else if (
            mode == MCTM_MODE_DEGRADED
        )
        {
            state =
                INFOTAINMENT_STATE_DEGRADED;

            workload_ms =
                INFOTAINMENT_DEGRADED_WORKLOAD_MS;

            g_degraded_cycles++;
        }
        else
        {
            state =
                INFOTAINMENT_STATE_NORMAL;

            workload_ms =
                INFOTAINMENT_NORMAL_WORKLOAD_MS;
        }


        /* === Next periodic release === */

        next_release +=
            INFOTAINMENT_PERIOD_MS;


        {
            uint64_t now =
                monotonic_ms();

            if (next_release > now)
            {
                usleep(
                    (useconds_t)(
                        (next_release - now) *
                        1000ULL
                    )
                );
            }
            else
            {
                next_release =
                    now;
            }
        }


        /* === Execute workload === */

        g_cycle++;
        g_total_cycles++;


        start_ms =
            monotonic_ms();

        start_ns =
            monotonic_ns();

        scheduling_log(
            start_ns,
            g_cycle,
            1,
            0.0,
            "PENDING",
            "PENDING",
            mode
        );

        run_workload(
            workload_ms
        );


        end_ms =
            monotonic_ms();

        end_ns =
            monotonic_ns();


        execution_ms =
            (double)(
                end_ms -
                start_ms
            );


        deadline_missed =
            execution_ms >
            INFOTAINMENT_PERIOD_MS;


        budget_overrun =
            execution_ms >
            INFOTAINMENT_BUDGET_MS;

        scheduling_log(
            end_ns,
            g_cycle,
            2,
            execution_ms,
            deadline_missed ? "MISS" : "OK",
            budget_overrun ? "OVERRUN" : "OK",
            mode
        );


        printf(
            "[INFOTAINMENT] "
            "Cycle=%llu | "
            "State=%s | "
            "MCTM=%s | "
            "Exec=%.3f ms | "
            "Budget=%d ms | "
            "Deadline=%s | "
            "BudgetStatus=%s\n",

            (unsigned long long)
                g_cycle,

            state_text(
                state
            ),

            mode_text(
                mode
            ),

            execution_ms,

            INFOTAINMENT_BUDGET_MS,

            deadline_missed
                ? "MISS"
                : "OK",

            budget_overrun
                ? "OVERRUN"
                : "OK"
        );

        fflush(stdout);
    }

    return NULL;
}


/* === Main === */

int main(void)
{
    name_attach_t *attach;

    int resource_coid;

    pthread_t ipc_thread;
    pthread_t workload_thread;

    struct sched_param param;


    {
        char value[40];

        mctm_dash_header("MCTM INFOTAINMENT TASK", "LOW-criticality workload");

        snprintf(value, sizeof(value), "%d", LOW_CRITICALITY_PRIORITY);
        mctm_dash_row("Priority", value);

        snprintf(value, sizeof(value), "%d ms", INFOTAINMENT_PERIOD_MS);
        mctm_dash_row("Period", value);

        snprintf(value, sizeof(value), "%d ms", INFOTAINMENT_NORMAL_WORKLOAD_MS);
        mctm_dash_row("Normal workload", value);

        snprintf(value, sizeof(value), "%d ms", INFOTAINMENT_DEGRADED_WORKLOAD_MS);
        mctm_dash_row("Degraded workload", value);

        mctm_dash_row("IPC peer", "resource_monitor");
        mctm_dash_row("Trace", SCHEDULE_TRACE_FILE);
        mctm_dash_footer();
    }


    /* === Main thread scheduling === */

    memset(
        &param,
        0,
        sizeof(param)
    );

    param.sched_priority =
        LOW_CRITICALITY_PRIORITY;

    if (
        pthread_setschedparam(
            pthread_self(),
            SCHED_FIFO,
            &param
        ) != 0
    )
    {
        perror(
            "[INFOTAINMENT] pthread_setschedparam"
        );
    }


    /* === Create named channel === */

    attach =
        name_attach(
            NULL,
            INFOTAINMENT_CHANNEL_NAME,
            0
        );

    if (attach == NULL)
    {
        printf(
            "[INFOTAINMENT] "
            "name_attach(%s) failed: %s\n",
            INFOTAINMENT_CHANNEL_NAME,
            strerror(errno)
        );

        return EXIT_FAILURE;
    }


    printf(
        "[INFOTAINMENT] Channel ready: %s\n",
        INFOTAINMENT_CHANNEL_NAME
    );


    /* === Connect ResourceMonitor === */

    resource_coid =
        name_open(
            RESOURCE_MONITOR_CHANNEL,
            0
        );

    if (resource_coid == -1)
    {
        printf(
            "[INFOTAINMENT] "
            "ResourceMonitor unavailable: %s\n",
            strerror(errno)
        );

        name_detach(
            attach,
            0
        );

        return EXIT_FAILURE;
    }


    printf(
        "[INFOTAINMENT] "
        "Connected to ResourceMonitor\n"
    );

    if (scheduling_logger_init() != 0)
    {
        printf(
            "[INFOTAINMENT] WARNING: scheduling trace unavailable: %s\n",
            strerror(errno)
        );
    }
    else
    {
        printf(
            "[INFOTAINMENT] Scheduling trace: %s\n",
            SCHEDULE_TRACE_FILE
        );
    }


    /* === Request current decision === */

    request_initial_decision(
        resource_coid
    );


    /* === Start IPC thread === */

    if (
        pthread_create(
            &ipc_thread,
            NULL,
            ipc_thread_func,
            attach
        ) != 0
    )
    {
        perror(
            "[INFOTAINMENT] "
            "pthread_create IPC"
        );

        name_close(
            resource_coid
        );

        name_detach(
            attach,
            0
        );

        return EXIT_FAILURE;
    }


    /* === Start workload thread === */

    if (
        pthread_create(
            &workload_thread,
            NULL,
            workload_thread_func,
            NULL
        ) != 0
    )
    {
        perror(
            "[INFOTAINMENT] "
            "pthread_create workload"
        );

        return EXIT_FAILURE;
    }


    /* === Keep main alive. === */

    pthread_join(
        ipc_thread,
        NULL
    );

    pthread_join(
        workload_thread,
        NULL
    );

    scheduling_logger_close();


    name_close(
        resource_coid
    );

    name_detach(
        attach,
        0
    );

    return EXIT_SUCCESS;
}
