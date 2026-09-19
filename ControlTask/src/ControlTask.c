/* SPDX-License-Identifier: MIT
 *
 * HyperSafe - ControlTask
 *
 * High-criticality deterministic control workload.
 *
 * Architecture:
 *
 *     SafetyTask
 *         |
 *         | SafetyStatusMessage
 *         v
 *     ControlTask
 *         |
 *         | ControlStatusMessage
 *         v
 *     ResourceMonitor
 *         |
 *         | MCTMDecisionMessage
 *         v
 *     ControlTask
 *
 * Design note:
 *   BMP180 has been removed from ControlTask.
 *   ControlTask no longer depends on I2C, /dev/i2c0,
 *   BMP180.h, or a physical temperature sensor.
 *
 *   ControlTask is now a deterministic software workload
 *   whose execution intensity can be changed by MCTM state
 *   and later by the software-only FaultInjector.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include "mctm_dashboard.h"

#include "mctm_config.h"
#include "mctm_types.h"
#include "mctm_messages.h"
#include "fault_injector.h"
#include "fault_reader.h"


/* === QNX IPC / Task Configuration === */

#define RESOURCE_MONITOR_CHANNEL  "resource_monitor"
#define CONTROL_CHANNEL_NAME      "control_task"

#define CONTROL_TASK_PRIORITY            25
#define CONTROL_TASK_LOW_PRIORITY        10
#define CONTROL_TASK_SPORADIC_MAX_REPL    4
#define CONTROL_TASK_CRITICALITY         "HIGH"

#define CONTROL_WORKLOAD_IDLE_MS   0


/* =========================================================
 * Messages received by ControlTask
 *
 * ControlTask receives two message types on its QNX channel:
 *   1. SafetyStatusMessage from SafetyTask
 *   2. MCTMDecisionMessage from ResourceMonitor
 * ========================================================= */

typedef union
{
    SafetyStatusMessage   safety;
    MCTMDecisionMessage  decision;
} ControlRxMessage;


/* === MCTM / Control state === */

static MCTMDecisionMessage g_mctm_decision;

static volatile int g_mctm_mode =
    MCTM_MODE_NORMAL;

static volatile int g_protect_safety =
    1;

static volatile int g_protect_control =
    1;

static volatile int g_allow_low_criticality =
    1;

static volatile int g_mctm_decision_valid =
    0;

static FaultReader g_fault_reader;


/* === Scheduling trace === */

static int g_schedule_trace_fd = -1;


/* === Timing helper === */

static uint64_t get_time_ns(void)
{
    struct timespec ts;

    if (clock_gettime(
            CLOCK_MONOTONIC,
            &ts) == -1)
    {
        return 0;
    }

    return
        ((uint64_t)ts.tv_sec * 1000000000ULL) +
        (uint64_t)ts.tv_nsec;
}


/* === MCTM text helpers === */

static const char *safety_text(
        SafetyState state)
{
    switch (state)
    {
        case SAFETY_WARNING:
            return "WARNING";

        case SAFETY_CRITICAL:
            return "CRITICAL";

        case SAFETY_NORMAL:
        default:
            return "NORMAL";
    }
}


static const char *control_text(
        ControlState state)
{
    switch (state)
    {
        case CONTROL_CAUTION:
            return "CAUTION";

        case CONTROL_EMERGENCY:
            return "EMERGENCY";

        case CONTROL_NORMAL:
        default:
            return "NORMAL";
    }
}


static const char *reason_text(
        SafetyReason reason)
{
    switch (reason)
    {
        case REASON_HIGH_TEMPERATURE:
            return "HIGH_TEMPERATURE";

        case REASON_COLLISION_RISK:
            return "COLLISION_RISK";

        case REASON_BRAKE_ANOMALY:
            return "BRAKE_ANOMALY";

        case REASON_CRITICAL_BRAKING:
            return "CRITICAL_BRAKING";

        case REASON_NONE:
        default:
            return "NONE";
    }
}

static const char *fault_text(int fault_type)
{
    switch (fault_type)
    {
        case FAULT_CPU_OVERLOAD: return "CPU_OVERLOAD";
        case FAULT_CONTROL_DELAY: return "CONTROL_DELAY";
        case FAULT_CONTROL_DEADLINE: return "CONTROL_DEADLINE";
        case FAULT_STALE_STATUS: return "STALE_STATUS";
        case FAULT_MISSING_STATUS: return "MISSING_STATUS";
        case FAULT_SENSOR_INVALID: return "SENSOR_INVALID";
        case FAULT_INFOTAINMENT_LOAD: return "INFOTAINMENT_LOAD";
        case FAULT_CONTROL_25MS: return "CONTROL_25MS";
        case FAULT_CLEAR:
        default: return "NONE";
    }
}


static const char *mctm_mode_text(
        int mode)
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


/* === Scheduling logger === */

static int scheduling_logger_init(void)
{
    const char *header =
        "timestamp_ns,cycle,task,event,"
        "execution_ms,deadline,budget,mode,criticality\n";

    g_schedule_trace_fd =
        open(
            SCHEDULE_TRACE_FILE,
            O_WRONLY | O_CREAT | O_APPEND,
            0666
        );

    if (g_schedule_trace_fd == -1)
        return -1;

    /*
     * The file header is written by the existing logger
     * implementation when appropriate. Keep this function
     * minimal and deterministic.
     */
    struct stat st;

    if (fstat(
            g_schedule_trace_fd,
            &st) == 0 &&
        st.st_size == 0)
    {
        size_t n = strlen(header);

        if (write(
                g_schedule_trace_fd,
                header,
                n) != (ssize_t)n)
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
        const char *event,
        double execution_ms,
        const char *deadline,
        const char *budget,
        int mode)
{
    char line[512];
    int len;

    if (g_schedule_trace_fd == -1)
        return;

    len =
        snprintf(
            line,
            sizeof(line),
            "%llu,%llu,ControlTask,%s,%.3f,%s,%s,%s,HIGH\n",
            (unsigned long long)timestamp_ns,
            (unsigned long long)cycle,
            event,
            execution_ms,
            deadline,
            budget,
            mctm_mode_text(mode)
        );

    if (len > 0)
    {
        (void)write(
            g_schedule_trace_fd,
            line,
            (size_t)len
        );
    }
}


static void scheduling_logger_close(void)
{
    if (g_schedule_trace_fd != -1)
    {
        close(g_schedule_trace_fd);
        g_schedule_trace_fd = -1;
    }
}


/* === Scheduling configuration === */

static int configure_control_scheduling(void)
{
    struct sched_param param;
    struct sched_param verify;
    struct timespec repl_period;
    struct timespec init_budget;
    int policy;
    int rc;

    /*
     * QNX sporadic-server configuration:
     *
     *   High priority      = 25
     *   Low priority       = 10
     *   Initial budget     = 10 ms
     *   Replenish period   = 50 ms
     *   Max replenishments = 4
     *
     * This is the actual QNX SCHED_SPORADIC scheduler
     * configuration for the ControlTask thread.
     */

    memset(&param, 0, sizeof(param));
    memset(&verify, 0, sizeof(verify));

    repl_period.tv_sec  = CONTROL_PERIOD_MS / 1000;
    repl_period.tv_nsec = (long)(CONTROL_PERIOD_MS % 1000) * 1000000L;

    init_budget.tv_sec  = CONTROL_BUDGET_MS / 1000;
    init_budget.tv_nsec = (long)(CONTROL_BUDGET_MS % 1000) * 1000000L;

    param.sched_priority      = CONTROL_TASK_PRIORITY;
    param.sched_ss_low_priority = CONTROL_TASK_LOW_PRIORITY;
    param.sched_ss_max_repl   = CONTROL_TASK_SPORADIC_MAX_REPL;
    param.sched_ss_repl_period = repl_period;
    param.sched_ss_init_budget  = init_budget;

    /*
     * SchedSet(0, 0, ...) is a QNX kernel scheduling call.
     * tid == 0 selects the calling thread.
     */
    errno = 0;
    rc = SchedSet(
        0,
        0,
        SCHED_SPORADIC,
        &param
    );

    if (rc == -1)
    {
        printf(
            "[SCHED] ControlTask SCHED_SPORADIC failed: "
            "errno=%d (%s)\n",
            errno,
            strerror(errno)
        );

        return -1;
    }

    /*
     * Verify the actual kernel scheduler state using the QNX
     * SchedGet() kernel call.
     */
    errno = 0;
    policy = SchedGet(
        0,
        0,
        &verify
    );

    if (policy == -1)
    {
        printf(
            "[SCHED] ControlTask SchedGet() failed: "
            "errno=%d (%s)\n",
            errno,
            strerror(errno)
        );

        return -1;
    }

    if (
        policy != SCHED_SPORADIC ||
        verify.sched_priority != CONTROL_TASK_PRIORITY ||
        verify.sched_ss_low_priority != CONTROL_TASK_LOW_PRIORITY ||
        verify.sched_ss_max_repl != CONTROL_TASK_SPORADIC_MAX_REPL ||
        verify.sched_ss_repl_period.tv_sec != repl_period.tv_sec ||
        verify.sched_ss_repl_period.tv_nsec != repl_period.tv_nsec ||
        verify.sched_ss_init_budget.tv_sec != init_budget.tv_sec ||
        verify.sched_ss_init_budget.tv_nsec != init_budget.tv_nsec
    )
    {
        printf(
            "[SCHED] ControlTask SCHED_SPORADIC verification failed\n"
        );

        printf(
            "[SCHED] kernel: policy=%d high=%d low=%d "
            "max_repl=%d budget=%ld.%09ld period=%ld.%09ld\n",
            policy,
            verify.sched_priority,
            verify.sched_ss_low_priority,
            verify.sched_ss_max_repl,
            (long)verify.sched_ss_init_budget.tv_sec,
            verify.sched_ss_init_budget.tv_nsec,
            (long)verify.sched_ss_repl_period.tv_sec,
            verify.sched_ss_repl_period.tv_nsec
        );

        return -1;
    }

    printf(
        "[SCHED] ControlTask: SCHED_SPORADIC\n"
        "        Normal priority = %d\n"
        "        Low priority    = %d\n"
        "        Budget          = %d ms\n"
        "        Period          = %d ms\n"
        "        Max repl.       = %d\n"
        "        Criticality     = %s\n",
        verify.sched_priority,
        verify.sched_ss_low_priority,
        CONTROL_BUDGET_MS,
        CONTROL_PERIOD_MS,
        verify.sched_ss_max_repl,
        CONTROL_TASK_CRITICALITY
    );

    return 0;
}


/* === ResourceMonitor connection helper === */

static int connect_resource_monitor(void)
{
    int coid;

    printf("[CONTROL-IPC] Connecting to ResourceMonitor...\n");
    fflush(stdout);

    errno = 0;

    coid = name_open(RESOURCE_MONITOR_CHANNEL, 0);

    if (coid == -1)
    {
        int saved_errno = errno;

        printf(
            "[CONTROL-IPC] ResourceMonitor unavailable: "
            "errno=%d (%s)\n",
            saved_errno,
            strerror(saved_errno)
        );
        fflush(stdout);

        return -1;
    }

    printf(
        "[CONTROL-IPC] Connected to ResourceMonitor coid=%d\n",
        coid
    );
    fflush(stdout);

    return coid;
}

/* === Runtime sporadic-priority observation === */

/*
 * Read only the kernel's CURRENT scheduling priority.
 *
 * QNX distinguishes between:
 *   sched_priority    = configured/assigned priority
 *   sched_curpriority = priority currently used by the kernel
 *
 * For SCHED_SPORADIC, sched_curpriority is the field we use
 * to observe the runtime 25 -> 10 transition.
 */
static int read_current_priority(
        int *current_priority,
        int *policy)
{
    struct sched_param param;
    int sched_policy;

    if (current_priority == NULL ||
        policy == NULL)
    {
        return -1;
    }

    memset(&param, 0, sizeof(param));

    errno = 0;

    sched_policy =
        SchedGet(
            0,
            0,
            &param
        );

    if (sched_policy == -1)
        return -1;

    *current_priority =
        param.sched_curpriority;

    *policy =
        sched_policy;

    return 0;
}


/* =========================================================
 * Deterministic control workload
 *
 * The workload deliberately consumes CPU for a bounded time.
 *
 * NORMAL     -> normal workload
 * DEGRADED   -> constrained workload
 * EMERGENCY  -> protected/emergency workload
 *
 * FaultInjector will later be able to select explicit
 * pressure/overload profiles without physical hardware.
 * ========================================================= */

static void control_workload(
        int mctm_mode,
        const FaultInjectionState *fault,
        uint64_t cycle)
{
    uint64_t start =
        get_time_ns();

    uint64_t workload_ms =
        CONTROL_NORMAL_WORKLOAD_MS;

    uint64_t delay_ms = 0;

    /* === Normal MCTM workload policy. === */
    if (mctm_mode == MCTM_MODE_DEGRADED)
    {
        workload_ms =
            CONTROL_PRESSURE_WORKLOAD_MS;
    }
    else if (mctm_mode == MCTM_MODE_EMERGENCY)
    {
        workload_ms =
            CONTROL_NORMAL_WORKLOAD_MS;
    }

    /* === Software fault overrides. === */
    if (fault != NULL && fault->active)
    {
        if (fault->workload_ms > 0)
            workload_ms = fault->workload_ms;

        delay_ms =
            fault->delay_ms;
    }

    /*
     * Do not cap a fault-injected workload here.
     *
     * The purpose of CONTROL_OVERLOAD / CONTROL_DEADLINE is
     * to let ResourceMonitor observe the actual over-budget
     * or over-deadline execution.
     */
    {
        uint64_t last_poll_ms = UINT64_MAX;
        int previous_current_priority = -1;

        while (
            ((get_time_ns() - start) / 1000000ULL) <
            workload_ms
        )
        {
            uint64_t elapsed_ms =
                (get_time_ns() - start) / 1000000ULL;

            /*
             * Poll the QNX kernel once per millisecond.
             *
             * IMPORTANT:
             * The configured/assigned priority is always the
             * constant CONTROL_TASK_PRIORITY (25). We deliberately
             * do not print sched_priority from SchedGet() here,
             * because the only runtime quantity we want to observe
             * is sched_curpriority.
             */
            if (elapsed_ms != last_poll_ms)
            {
                int current_priority;
                int policy;

                last_poll_ms =
                    elapsed_ms;

                if (read_current_priority(
                        &current_priority,
                        &policy) == 0)
                {
                    if (current_priority !=
                        previous_current_priority)
                    {
                        printf(
                            "[SCHED-RUNTIME] cycle=%llu "
                            "policy=%s configured=%d "
                            "current=%d elapsed=%llums\n",

                            (unsigned long long)cycle,

                            policy == SCHED_SPORADIC
                                ? "SCHED_SPORADIC"
                                : "OTHER",

                            CONTROL_TASK_PRIORITY,

                            current_priority,

                            (unsigned long long)elapsed_ms
                        );

                        fflush(stdout);

                        previous_current_priority =
                            current_priority;
                    }
                }
            }
        }
    }

    /* === Optional injected delay. === */
    if (delay_ms > 0)
    {
        struct timespec ts;

        ts.tv_sec =
            (time_t)(delay_ms / 1000U);

        ts.tv_nsec =
            (long)((delay_ms % 1000U) * 1000000UL);

        nanosleep(
            &ts,
            NULL
        );
    }
}


/* === Control calculation === */

static void calculate_control(
        const SafetyStatusMessage *safety,
        int mctm_mode,
        int protect_safety,
        ControlState *control_state,
        double *target_speed,
        double *throttle,
        double *brake,
        int *emergency_stop)
{
    if (
        safety == NULL ||
        control_state == NULL ||
        target_speed == NULL ||
        throttle == NULL ||
        brake == NULL ||
        emergency_stop == NULL
    )
    {
        return;
    }


    /* === Default NORMAL control. === */
    *control_state =
        CONTROL_NORMAL;

    *target_speed =
        safety->vehicle_speed;

    *throttle =
        40.0;

    *brake =
        0.0;

    *emergency_stop =
        0;


    /* === EMERGENCY caused by MCTM or safety condition === */

    if (
        (mctm_mode == MCTM_MODE_EMERGENCY &&
         protect_safety) ||
        safety->safety_state == SAFETY_CRITICAL
    )
    {
        *control_state =
            CONTROL_EMERGENCY;

        *target_speed =
            0.0;

        *throttle =
            0.0;

        *brake =
            100.0;

        *emergency_stop =
            1;

        return;
    }


    /* === DEGRADED / CAUTION === */

    if (mctm_mode == MCTM_MODE_DEGRADED)
    {
        *control_state =
            CONTROL_CAUTION;

        *target_speed =
            safety->vehicle_speed - 10.0;

        if (*target_speed < 0.0)
            *target_speed = 0.0;

        *throttle =
            25.0;

        *brake =
            10.0;

        return;
    }


    /* === Explicit emergency mode without safety criticality === */

    if (mctm_mode == MCTM_MODE_EMERGENCY)
    {
        *control_state =
            CONTROL_EMERGENCY;

        *target_speed =
            0.0;

        *throttle =
            0.0;

        *brake =
            100.0;

        *emergency_stop =
            1;

        return;
    }


    /* === Safety warning === */

    if (safety->safety_state == SAFETY_WARNING)
    {
        *control_state =
            CONTROL_CAUTION;

        *target_speed =
            safety->vehicle_speed - 20.0;

        if (*target_speed < 0.0)
            *target_speed = 0.0;

        *throttle =
            15.0;

        *brake =
            30.0;
    }
}


/* === MAIN === */

int main(void)
{
    name_attach_t *attach;

    int resource_coid;
    int rcvid;

    uint64_t cycle = 0;
    uint64_t deadline_misses = 0;
    uint64_t budget_overruns = 0;


    /* === Scheduling === */

    if (
        configure_control_scheduling() != 0
    )
    {
        return EXIT_FAILURE;
    }


    /* === Scheduling logger === */

    if (
        scheduling_logger_init() != 0
    )
    {
        printf(
            "[SCHED-LOG] WARNING: trace unavailable\n"
        );
    }

    fault_reader_init(&g_fault_reader);


    /* === Startup banner === */

    {
        char value[32];

        mctm_dash_header("MCTM CONTROL TASK", "HIGH-criticality deterministic control loop");

        snprintf(value, sizeof(value), "%d (low=%d)",
                 CONTROL_TASK_PRIORITY, CONTROL_TASK_LOW_PRIORITY);
        mctm_dash_row("Priority", value);

        snprintf(value, sizeof(value), "%d / %d ms",
                 CONTROL_BUDGET_MS, CONTROL_PERIOD_MS);
        mctm_dash_row("Budget / period", value);

        snprintf(value, sizeof(value), "%d ms", CONTROL_DEADLINE_MS);
        mctm_dash_row("Deadline", value);

        mctm_dash_row("Scheduling", "SCHED_SPORADIC");
        mctm_dash_row("IPC peer", "resource_monitor");
        mctm_dash_row("Trace", SCHEDULE_TRACE_FILE);
        mctm_dash_row("Fault source", "/mctm_fault_injector");
        mctm_dash_footer();
    }


    /*
     * ---------------------------------------------------------
     * ControlTask named channel
     *
     * ResourceMonitor uses this channel to deliver
     * MCTMDecisionMessage.
     * ---------------------------------------------------------
     */

    attach =
        name_attach(
            NULL,
            CONTROL_CHANNEL_NAME,
            0
        );

    if (attach == NULL)
    {
        printf(
            "[IPC] name_attach(%s) failed: %s\n",
            CONTROL_CHANNEL_NAME,
            strerror(errno)
        );

        scheduling_logger_close();

        return EXIT_FAILURE;
    }


    printf(
        "[IPC] ControlTask channel ready: %s\n",
        CONTROL_CHANNEL_NAME
    );


    /* === Connect to ResourceMonitor === */

    resource_coid = connect_resource_monitor();

    if (resource_coid == -1)
    {
        printf(
            "[IPC] ResourceMonitor unavailable at startup.\n"
        );

        name_detach(
            attach,
            0
        );

        scheduling_logger_close();

        return EXIT_FAILURE;
    }


    /*
     * ---------------------------------------------------------
     * Main IPC loop
     * ---------------------------------------------------------
     *
     * ControlTask waits for SafetyStatusMessage from SafetyTask
     * or MCTMDecisionMessage from ResourceMonitor.
     * ---------------------------------------------------------
     */

    while (1)
    {
        ControlRxMessage rx;

        memset(
            &rx,
            0,
            sizeof(rx)
        );


        rcvid =
            MsgReceive(
                attach->chid,
                &rx,
                sizeof(rx),
                NULL
            );


        if (rcvid == -1)
        {
            if (errno == EINTR)
                continue;

            printf(
                "[IPC] MsgReceive failed: %s\n",
                strerror(errno)
            );

            continue;
        }


        /* === Ignore pulses for this application channel. === */
        if (rcvid == 0)
            continue;


        /* === MCTM DECISION from ResourceMonitor === */

        if (
            rx.safety.type ==
            MCTM_MSG_MCTM_DECISION
        )
        {
            memcpy(
                &g_mctm_decision,
                &rx.decision,
                sizeof(g_mctm_decision)
            );

            g_mctm_mode =
                g_mctm_decision.mctm_mode;

            g_protect_safety =
                g_mctm_decision.protect_safety;

            g_protect_control =
                g_mctm_decision.protect_control;

            g_allow_low_criticality =
                g_mctm_decision.allow_low_criticality;

            g_mctm_decision_valid =
                1;


            printf(
                "[CONTROL-IPC] "
                "MCTM=%s "
                "SafetyUtil=%.1f%% "
                "ControlUtil=%.1f%% "
                "LowCriticality=%s\n",

                mctm_mode_text(
                    g_mctm_mode
                ),

                g_mctm_decision.safety_utilization *
                    100.0,

                g_mctm_decision.control_utilization *
                    100.0,

                g_allow_low_criticality
                    ? "ALLOWED"
                    : "RESTRICTED"
            );


            /* === Reply to ResourceMonitor's synchronous message. === */
            {
                MCTMReply reply;

                reply.status = 0;

                MsgReply(
                    rcvid,
                    EOK,
                    &reply,
                    sizeof(reply)
                );
            }

            continue;
        }


        /* === Unexpected message === */

        if (
            rx.safety.type !=
            MCTM_MSG_SAFETY_STATUS
        )
        {
            MCTMReply reply;

            reply.status = -1;

            MsgReply(
                rcvid,
                EINVAL,
                &reply,
                sizeof(reply)
            );

            continue;
        }


        /* === SafetyStatusMessage received === */

        {
            SafetyStatusMessage safety_msg;
            ControlStatusMessage control_msg;
            MCTMReply reply;

            FaultInjectionState fault;
            int fault_available;
            int suppress_status;
            int synthetic_sensor_invalid;

            ControlState control_state;

            double target_speed;
            double throttle;
            double brake;

            int emergency_stop;

            uint64_t start_ns;
            uint64_t end_ns;

            double execution_time_ms;

            int deadline_missed;
            int budget_overrun;


            memcpy(
                &safety_msg,
                &rx.safety,
                sizeof(safety_msg)
            );

            memset(
                &fault,
                0,
                sizeof(fault)
            );

            fault_available =
                (fault_reader_read(
                    &g_fault_reader,
                    &fault
                ) == 0);

            suppress_status =
                fault_available &&
                fault.active &&
                fault.suppress_status;

            synthetic_sensor_invalid =
                fault_available &&
                fault.active &&
                fault.synthetic_sensor_invalid;

            if (synthetic_sensor_invalid)
            {
                safety_msg.obstacle_distance =
                    -1.0;
            }

            cycle++;


            /* === Start timing. === */
            start_ns =
                get_time_ns();

            scheduling_log(
                start_ns,
                cycle,
                "START",
                0.0,
                "PENDING",
                "PENDING",
                g_mctm_mode
            );


            /* === Calculate control response. === */
            calculate_control(
                &safety_msg,
                g_mctm_mode,
                g_protect_safety,
                &control_state,
                &target_speed,
                &throttle,
                &brake,
                &emergency_stop
            );


            /* === Deterministic CPU workload. === */
            control_workload(
                g_mctm_mode,
                fault_available && fault.active
                    ? &fault
                    : NULL,
                cycle
            );


            /* === End timing. === */
            end_ns =
                get_time_ns();

            execution_time_ms =
                (double)(
                    end_ns - start_ns
                ) / 1000000.0;


            /* === Timing checks. === */
            deadline_missed =
                execution_time_ms >
                CONTROL_DEADLINE_MS;

            budget_overrun =
                execution_time_ms >
                CONTROL_BUDGET_MS;


            if (deadline_missed)
                deadline_misses++;

            if (budget_overrun)
                budget_overruns++;


            scheduling_log(
                end_ns,
                cycle,
                "END",
                execution_time_ms,
                deadline_missed
                    ? "MISS"
                    : "OK",
                budget_overrun
                    ? "OVERRUN"
                    : "OK",
                g_mctm_mode
            );


            /* === Construct ControlStatusMessage === */

            memset(
                &control_msg,
                0,
                sizeof(control_msg)
            );

            control_msg.type =
                MCTM_MSG_CONTROL_STATUS;

            control_msg.cycle =
                cycle;

            control_msg.criticality =
                CRITICALITY_HIGH;

            control_msg.control_state =
                control_state;

            control_msg.execution_time_ms =
                execution_time_ms;

            control_msg.budget_ms =
                CONTROL_BUDGET_MS;

            control_msg.deadline_missed =
                deadline_missed;

            control_msg.budget_overrun =
                budget_overrun;

            control_msg.target_speed =
                target_speed;

            control_msg.throttle_percent =
                throttle;

            control_msg.brake_percent =
                brake;


            /* === Send ControlStatusMessage to ResourceMonitor === */

            reply.status = 0;

            if (!suppress_status)
            {
                int send_rc = MsgSend(
                    resource_coid,
                    &control_msg,
                    sizeof(control_msg),
                    &reply,
                    sizeof(reply)
                );

                if (send_rc == -1)
                {
                    int saved_errno = errno;

                    printf(
                        "[CONTROL] ResourceMonitor MsgSend failed: "
                        "errno=%d (%s)\n",
                        saved_errno,
                        strerror(saved_errno)
                    );
                    fflush(stdout);

                    /*
                     * ResourceMonitor may have been restarted after
                     * ControlTask established the original connection.
                     * Reconnect on a stale QNX connection and retry once.
                     */
                    if (saved_errno == EBADF ||
                        saved_errno == ESRCH ||
                        saved_errno == ENXIO ||
                        saved_errno == EPIPE)
                    {
                        printf(
                            "[CONTROL-IPC] ResourceMonitor connection "
                            "is stale. Reconnecting...\n"
                        );
                        fflush(stdout);

                        name_close(resource_coid);
                        resource_coid = -1;

                        resource_coid = connect_resource_monitor();

                        if (resource_coid != -1)
                        {
                            send_rc = MsgSend(
                                resource_coid,
                                &control_msg,
                                sizeof(control_msg),
                                &reply,
                                sizeof(reply)
                            );

                            if (send_rc == 0)
                            {
                                printf(
                                    "[CONTROL-IPC] Reconnected to "
                                    "ResourceMonitor and resent "
                                    "ControlStatus successfully.\n"
                                );
                            }
                            else
                            {
                                int retry_errno = errno;

                                printf(
                                    "[CONTROL-IPC] Retry MsgSend failed: "
                                    "errno=%d (%s)\n",
                                    retry_errno,
                                    strerror(retry_errno)
                                );
                            }

                            fflush(stdout);
                        }
                    }
                }
                else
                {
                }
            }
            else
            {
                printf("[CONTROL-FAULT] Status publication suppressed (type=%d / %s)\n",
                       fault.fault_type, fault_text(fault.fault_type));
                fflush(stdout);
            }

                        /* === CLI output === */

            printf(
                "[CONTROL] %s | Cycle=%llu | MCTM=%s | Safety=%s | "
                "Target=%.1f km/h | Throttle=%.1f%% | Brake=%.1f%% | "
                "Emergency=%s | Exec=%.3f ms | Deadline=%s | Budget=%s\n",
                control_text(control_state),
                (unsigned long long)safety_msg.cycle,
                mctm_mode_text(g_mctm_mode),
                safety_text(safety_msg.safety_state),
                target_speed,
                throttle,
                brake,
                emergency_stop ? "YES" : "NO",
                execution_time_ms,
                deadline_missed ? "MISS" : "OK",
                budget_overrun ? "OVERRUN" : "OK"
            );
            fflush(stdout);

            printf(
                "          SafetyReason=%s | Fault=%s | "
                "TotalDeadlineMisses=%llu | TotalBudgetOverruns=%llu\n",
                reason_text(safety_msg.safety_reason),
                (fault_available && fault.active)
                    ? fault_text(fault.fault_type)
                    : "NONE",
                (unsigned long long)deadline_misses,
                (unsigned long long)budget_overruns
            );
            fflush(stdout);

            reply.status = 0;

            if (MsgReply(
                    rcvid,
                    EOK,
                    &reply,
                    sizeof(reply)
                ) == -1)
            {
                printf("[CONTROL-IPC] MsgReply failed: %s\n", strerror(errno));
                fflush(stdout);
            }

        }
    }


    /* === Cleanup === */

    fault_reader_close(&g_fault_reader);

    name_close(
        resource_coid
    );

    name_detach(
        attach,
        0
    );

    scheduling_logger_close();

    return EXIT_SUCCESS;
}
