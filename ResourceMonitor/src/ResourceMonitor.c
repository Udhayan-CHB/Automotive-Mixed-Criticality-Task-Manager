/*
 * SPDX-License-Identifier: MIT
 *
 * HyperSafe - ResourceMonitor
 *
 * MCTM resource-management decision engine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>

#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include "mctm_dashboard.h"

#include "mctm_config.h"
#include "mctm_messages.h"
#include "resource_monitor.h"
#include "safety_monitor_msg.h"
#include "telemetry_msg.h"


/*
 * ---------------------------------------------------------
 * Canonical protocol
 * ---------------------------------------------------------
 *
 * SafetyTask sends raw SafetyStatusMessage.
 * ControlTask sends raw ControlStatusMessage.
 *
 * Both messages already contain their canonical type:
 *
 *   MCTM_MSG_SAFETY_STATUS  = 1
 *   MCTM_MSG_CONTROL_STATUS = 3
 *
 * ResourceMonitor therefore receives those structures
 * directly and dispatches on their type.
 * ---------------------------------------------------------
 */


/* === Named channels === */

#define RESOURCE_MONITOR_CHANNEL "resource_monitor"
#define CONTROL_CHANNEL_NAME     "control_task"
#define SAFETY_MONITOR_CHANNEL   "SafetyMonitor"

/*
 * Important:
 *
 * This must exactly match InfotainmentTask.
 *
 * InfotainmentTask currently uses:
 *
 *     #define INFOTAINMENT_CHANNEL_NAME "infotainment"
 *
 * Therefore ResourceMonitor must use "infotainment".
 */
#define INFOTAINMENT_CHANNEL     "infotainment"


/* === Dashboard === */

#define DASHBOARD_STATUS_PATH      \
    "/tmp/mctm_dashboard_status.json"

#define DASHBOARD_STATUS_TMP_PATH  \
    "/tmp/mctm_dashboard_status.json.tmp"


/* === Bounded MsgSend timeout === */

#define RM_SEND_TIMEOUT_NS \
    (20ULL * 1000000ULL)


/* === Global QNX state === */

static int g_chid = -1;
static int g_running = 1;


/* === ResourceMonitor -> ControlTask === */
static int g_control_coid = -1;


/* === ResourceMonitor -> SafetyMonitor === */
static int g_safetymon_coid = -1;


/* === ResourceMonitor -> InfotainmentTask === */
static int g_infotainment_coid = -1;

/* ResourceMonitor -> TelemetryBridge */
static int g_telemetry_coid = -1;


/* === Latest task status === */
static SafetyStatusMessage g_last_safety;
static ControlStatusMessage g_last_control;


/* === Availability flags === */
static int g_have_safety = 0;
static int g_have_control = 0;


/* === Current MCTM decision === */
static MCTMDecisionMessage g_current_decision;

/*
 * Last MCTM policy dispatched to downstream consumers.
 * Only the policy fields are compared; cycle/utilization changes
 * are telemetry and must not cause duplicate command pushes.
 */
static MCTMDecisionMessage g_last_dispatched_decision;
static int g_have_dispatched_decision = 0;


/* === Utilization === */

static double calculate_utilization(
        double execution_ms,
        double period_ms)
{
    if (period_ms <= 0.0)
        return 0.0;

    return execution_ms / period_ms;
}


/* === MCTM mode text === */

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


/* === Core MCTM decision engine === */

void resource_monitor_evaluate(
        const ResourceSnapshot *snapshot,
        MCTMDecisionMessage *decision)
{
    int mode = MCTM_MODE_NORMAL;


    if (decision == NULL)
        return;


    memset(
        decision,
        0,
        sizeof(*decision)
    );


    decision->type =
        MCTM_MSG_MCTM_DECISION;

    /* === Safety and Control are always protected. === */
    decision->protect_safety = 1;
    decision->protect_control = 1;


    if (snapshot == NULL)
    {
        decision->mctm_mode =
            MCTM_MODE_EMERGENCY;

        decision->allow_low_criticality =
            0;

        return;
    }


    decision->safety_utilization =
        snapshot->safety_utilization;

    decision->control_utilization =
        snapshot->control_utilization;


    /* === EMERGENCY === */

    if (
        snapshot->safety_state ==
            SAFETY_CRITICAL
        ||
        snapshot->safety_deadline_missed
        ||
        snapshot->control_deadline_missed
        ||
        snapshot->safety_utilization >=
            RM_CRITICAL_UTILIZATION
        ||
        snapshot->control_utilization >=
            RM_CRITICAL_UTILIZATION
    )
    {
        mode =
            MCTM_MODE_EMERGENCY;
    }


    /* === DEGRADED === */

    else if (
        snapshot->safety_state ==
            SAFETY_WARNING
        ||
        snapshot->safety_budget_overrun
        ||
        snapshot->control_budget_overrun
        ||
        snapshot->safety_utilization >=
            RM_WARNING_UTILIZATION
        ||
        snapshot->control_utilization >=
            RM_WARNING_UTILIZATION
    )
    {
        mode =
            MCTM_MODE_DEGRADED;
    }


    decision->mctm_mode =
        mode;


    /*
     * -----------------------------------------------------
     * IMPORTANT MIXED-CRITICALITY POLICY
     *
     * NORMAL:
     *     low-criticality allowed at full workload.
     *
     * DEGRADED:
     *     low-criticality still allowed, but
     *     InfotainmentTask itself reduces its workload.
     *
     * EMERGENCY:
     *     low-criticality suppressed.
     * -----------------------------------------------------
     */

    decision->allow_low_criticality =
        (mode != MCTM_MODE_EMERGENCY)
            ? 1
            : 0;
}


/* === Evaluate current status === */

static void evaluate_decision(void)
{
    ResourceSnapshot snapshot;


    memset(
        &snapshot,
        0,
        sizeof(snapshot)
    );


    /*
     * Until both high-criticality tasks have reported,
     * stay conservative.
     */
    if (
        !g_have_safety ||
        !g_have_control
    )
    {
        memset(
            &g_current_decision,
            0,
            sizeof(g_current_decision)
        );

        g_current_decision.type =
            MCTM_MSG_MCTM_DECISION;

        g_current_decision.cycle =
            g_have_control
                ? g_last_control.cycle
                : g_last_safety.cycle;

        g_current_decision.mctm_mode =
            MCTM_MODE_DEGRADED;

        g_current_decision.protect_safety =
            1;

        g_current_decision.protect_control =
            1;

        /*
         * Startup is conservative:
         * do not permit low-criticality until both
         * safety and control are reporting.
         */
        g_current_decision.allow_low_criticality =
            0;

        return;
    }


    /* === Safety period = 100 ms. === */
    snapshot.safety_utilization =
        calculate_utilization(
            g_last_safety.execution_time_ms,
            100.0
        );


    /* === Control period = 50 ms. === */
    snapshot.control_utilization =
        calculate_utilization(
            g_last_control.execution_time_ms,
            50.0
        );


    snapshot.safety_deadline_missed =
        g_last_safety.deadline_missed;

    snapshot.safety_budget_overrun =
        g_last_safety.budget_overrun;


    snapshot.control_deadline_missed =
        g_last_control.deadline_missed;

    snapshot.control_budget_overrun =
        g_last_control.budget_overrun;


    snapshot.safety_state =
        g_last_safety.safety_state;

    snapshot.control_state =
        g_last_control.control_state;


    resource_monitor_evaluate(
        &snapshot,
        &g_current_decision
    );


    /* === Most recent cycle. === */
    g_current_decision.cycle =
        g_last_control.cycle;
}


/*
 * ---------------------------------------------------------
 * MCTM policy-change detection
 * ---------------------------------------------------------
 * A new decision is dispatched only when one of the fields that
 * changes downstream behavior has changed.
 *
 * Cycle and utilization are monitoring data, not command state,
 * so they deliberately do not trigger a duplicate push.
 */
static int decision_policy_changed(
        const MCTMDecisionMessage *old_decision,
        const MCTMDecisionMessage *new_decision)
{
    if (old_decision == NULL || new_decision == NULL)
        return 1;

    if (old_decision->mctm_mode != new_decision->mctm_mode)
        return 1;

    if (old_decision->protect_safety != new_decision->protect_safety)
        return 1;

    if (old_decision->protect_control != new_decision->protect_control)
        return 1;

    if (old_decision->allow_low_criticality !=
        new_decision->allow_low_criticality)
        return 1;

    return 0;
}


/* === Bounded MsgSend === */

static int send_with_timeout(
        int coid,
        const void *msg,
        size_t msg_size,
        void *reply,
        size_t reply_size)
{
    uint64_t timeout_ns =
        RM_SEND_TIMEOUT_NS;


    (void)TimerTimeout(
        CLOCK_MONOTONIC,
        _NTO_TIMEOUT_SEND |
        _NTO_TIMEOUT_REPLY,
        NULL,
        &timeout_ns,
        NULL
    );


    return MsgSend(
        coid,
        msg,
        msg_size,
        reply,
        reply_size
    );
}


/* === Push MCTM decision to ControlTask === */

static void push_decision_to_control(void)
{
    MCTMReply reply;


    if (g_control_coid == -1)
    {
        g_control_coid =
            name_open(
                CONTROL_CHANNEL_NAME,
                0
            );


        if (g_control_coid == -1)
        {
            printf(
                "[MCTM] ControlTask not reachable yet: %s\n",
                strerror(errno)
            );

            fflush(stdout);

            return;
        }


        printf(
            "[MCTM] Connected to ControlTask "
            "for decision push\n"
        );

        fflush(stdout);
    }


    memset(
        &reply,
        0,
        sizeof(reply)
    );


    if (
        send_with_timeout(
            g_control_coid,
            &g_current_decision,
            sizeof(g_current_decision),
            &reply,
            sizeof(reply)
        ) == -1
    )
    {
        printf(
            "[MCTM] Decision push to ControlTask failed: %s\n",
            strerror(errno)
        );

        fflush(stdout);


        name_close(
            g_control_coid
        );

        g_control_coid = -1;
    }
}


/* === Push supervision snapshot to SafetyMonitor === */

/* === Push supervision snapshot to SafetyMonitor === */
static void push_snapshot_to_safetymon(void)
{
    SafetyMonitorSnapshot snapshot;
    SafetyMonitorReply reply;
    int rc;

    /* ---------------------------------------------------------
     * Connect to SafetyMonitor if necessary
     * --------------------------------------------------------- */
    if (g_safetymon_coid == -1)
    {
        g_safetymon_coid =
            name_open(SAFETY_MONITOR_CHANNEL, 0);

        if (g_safetymon_coid == -1)
        {
            /* SafetyMonitor is optional for MCTM operation */
            return;
        }

        printf("[MCTM] Connected to SafetyMonitor\n");
        fflush(stdout);
    }

    /* ---------------------------------------------------------
     * Construct canonical snapshot
     * --------------------------------------------------------- */
    memset(&snapshot, 0, sizeof(snapshot));

    snapshot.type = SAFETYMON_MSG_SNAPSHOT;

    {
        struct timespec ts;

        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        {
            snapshot.timestamp_ns =
                ((uint64_t)ts.tv_sec * 1000000000ULL) +
                (uint64_t)ts.tv_nsec;
        }
    }

    snapshot.safety_cycle =
        g_last_safety.cycle;

    snapshot.control_cycle =
        g_last_control.cycle;

    snapshot.safety_state =
        g_last_safety.safety_state;

    snapshot.control_state =
        g_last_control.control_state;

    snapshot.safety_execution_ms =
        g_last_safety.execution_time_ms;

    snapshot.control_execution_ms =
        g_last_control.execution_time_ms;

    snapshot.safety_deadline_missed =
        g_last_safety.deadline_missed;

    snapshot.safety_budget_overrun =
        g_last_safety.budget_overrun;

    snapshot.control_deadline_missed =
        g_last_control.deadline_missed;

    snapshot.control_budget_overrun =
        g_last_control.budget_overrun;

    snapshot.mctm_mode =
        g_current_decision.mctm_mode;

    snapshot.have_safety =
        g_have_safety;

    snapshot.have_control =
        g_have_control;


    /* ---------------------------------------------------------
     * Send snapshot to SafetyMonitor
     * --------------------------------------------------------- */
    memset(&reply, 0, sizeof(reply));

    rc = send_with_timeout(
        g_safetymon_coid,
        &snapshot,
        sizeof(snapshot),
        &reply,
        sizeof(reply)
    );

    if (rc == -1)
    {
        printf(
            "[MCTM] SafetyMonitor push failed: %s\n",
            strerror(errno)
        );

        fflush(stdout);

        name_close(g_safetymon_coid);
        g_safetymon_coid = -1;
    }
}


/* === Push MCTM decision to InfotainmentTask === */

static void push_decision_to_infotainment(void)
{
    MCTMReply reply;


    if (g_infotainment_coid == -1)
    {
        g_infotainment_coid =
            name_open(
                INFOTAINMENT_CHANNEL,
                0
            );


        if (g_infotainment_coid == -1)
            return;


        printf(
            "[MCTM] Connected to InfotainmentTask\n"
        );

        fflush(stdout);
    }


    memset(
        &reply,
        0,
        sizeof(reply)
    );


    if (
        send_with_timeout(
            g_infotainment_coid,
            &g_current_decision,
            sizeof(g_current_decision),
            &reply,
            sizeof(reply)
        ) == -1
    )
    {
        printf(
            "[MCTM] Decision push to InfotainmentTask "
            "failed: %s\n",
            strerror(errno)
        );

        fflush(stdout);


        name_close(
            g_infotainment_coid
        );

        g_infotainment_coid = -1;
    }
}


/* === Dashboard snapshot === */


/* === Push consolidated telemetry snapshot to TelemetryBridge === */
static void push_snapshot_to_telemetry(void)
{
    HyperSafeTelemetry snapshot;
    TelemetryReply reply;
    struct timespec ts;
    int rc;

    if (g_telemetry_coid == -1)
    {
        g_telemetry_coid =
            name_open(TELEMETRY_BRIDGE_CHANNEL, 0);

        if (g_telemetry_coid == -1)
        {
            /* TelemetryBridge is optional for MCTM operation. */
            return;
        }

        printf("[MCTM] Connected to TelemetryBridge\n");
        fflush(stdout);
    }

    memset(&snapshot, 0, sizeof(snapshot));

    snapshot.type = TELEMETRY_MSG_SNAPSHOT;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    {
        snapshot.timestamp_ns =
            ((uint64_t)ts.tv_sec * 1000000000ULL) +
            (uint64_t)ts.tv_nsec;
    }

    snapshot.cycle =
        g_current_decision.cycle;

    snapshot.safety_state =
        g_last_safety.safety_state;
    snapshot.safety_reason =
        g_last_safety.safety_reason;
    snapshot.temperature =
        g_last_safety.temperature;
    snapshot.brake_pressure =
        g_last_safety.brake_pressure;
    snapshot.obstacle_distance =
        g_last_safety.obstacle_distance;
    snapshot.vehicle_speed =
        g_last_safety.vehicle_speed;
    snapshot.brake_pressed =
        g_last_safety.brake_pressed;

    snapshot.safety_execution_ms =
        g_last_safety.execution_time_ms;
    snapshot.safety_deadline_missed =
        g_last_safety.deadline_missed;
    snapshot.safety_budget_overrun =
        g_last_safety.budget_overrun;

    snapshot.control_state =
        g_last_control.control_state;
    snapshot.control_execution_ms =
        g_last_control.execution_time_ms;
    snapshot.control_deadline_missed =
        g_last_control.deadline_missed;
    snapshot.control_budget_overrun =
        g_last_control.budget_overrun;

    snapshot.mctm_mode =
        g_current_decision.mctm_mode;
    snapshot.protect_safety =
        g_current_decision.protect_safety;
    snapshot.protect_control =
        g_current_decision.protect_control;
    snapshot.allow_low_criticality =
        g_current_decision.allow_low_criticality;

    snapshot.safety_utilization =
        g_current_decision.safety_utilization;
    snapshot.control_utilization =
        g_current_decision.control_utilization;

    snapshot.have_safety =
        g_have_safety;
    snapshot.have_control =
        g_have_control;

    memset(&reply, 0, sizeof(reply));

    rc = send_with_timeout(
        g_telemetry_coid,
        &snapshot,
        sizeof(snapshot),
        &reply,
        sizeof(reply));

    if (rc == -1)
    {
        printf("[MCTM] TelemetryBridge push failed: %s\n",
               strerror(errno));
        fflush(stdout);

        name_close(g_telemetry_coid);
        g_telemetry_coid = -1;
    }
}

static void write_dashboard_snapshot(void)
{
    FILE *f;

    struct timespec ts;

    uint64_t now_ms;


    f =
        fopen(
            DASHBOARD_STATUS_TMP_PATH,
            "w"
        );


    if (f == NULL)
        return;


    clock_gettime(
        CLOCK_MONOTONIC,
        &ts
    );


    now_ms =
        ((uint64_t)ts.tv_sec * 1000ULL)
        +
        ((uint64_t)ts.tv_nsec /
            1000000ULL);


    fprintf(
        f,
        "{\n"
        "  \"timestamp_ms\": %llu,\n"
        "  \"mctm\": {\n"
        "    \"mode\": \"%s\",\n"
        "    \"cycle\": %llu,\n"
        "    \"safety_utilization\": %.4f,\n"
        "    \"control_utilization\": %.4f,\n"
        "    \"protect_safety\": %s,\n"
        "    \"protect_control\": %s,\n"
        "    \"allow_low_criticality\": %s\n"
        "  },\n"
        "  \"safety\": {\n"
        "    \"have_data\": %s,\n"
        "    \"cycle\": %llu,\n"
        "    \"state\": %d,\n"
        "    \"reason\": %d,\n"
        "    \"temperature\": %.2f,\n"
        "    \"brake_pressure\": %.2f,\n"
        "    \"obstacle_distance_m\": %.3f,\n"
        "    \"vehicle_speed\": %.2f,\n"
        "    \"brake_pressed\": %s,\n"
        "    \"execution_time_ms\": %.3f,\n"
        "    \"deadline_missed\": %s,\n"
        "    \"budget_overrun\": %s\n"
        "  },\n"
        "  \"control\": {\n"
        "    \"have_data\": %s,\n"
        "    \"cycle\": %llu,\n"
        "    \"state\": %d,\n"
        "    \"execution_time_ms\": %.3f,\n"
        "    \"budget_ms\": %.2f,\n"
        "    \"deadline_missed\": %s,\n"
        "    \"budget_overrun\": %s,\n"
        "    \"target_speed\": %.2f,\n"
        "    \"throttle_percent\": %.2f,\n"
        "    \"brake_percent\": %.2f\n"
        "  }\n"
        "}\n",

        (unsigned long long)now_ms,

        mode_text(
            g_current_decision.mctm_mode
        ),

        (unsigned long long)
            g_current_decision.cycle,

        g_current_decision.safety_utilization,

        g_current_decision.control_utilization,

        g_current_decision.protect_safety
            ? "true"
            : "false",

        g_current_decision.protect_control
            ? "true"
            : "false",

        g_current_decision.allow_low_criticality
            ? "true"
            : "false",

        g_have_safety
            ? "true"
            : "false",

        (unsigned long long)
            g_last_safety.cycle,

        (int)
            g_last_safety.safety_state,

        (int)
            g_last_safety.safety_reason,

        g_last_safety.temperature,

        g_last_safety.brake_pressure,

        g_last_safety.obstacle_distance,

        g_last_safety.vehicle_speed,

        g_last_safety.brake_pressed
            ? "true"
            : "false",

        g_last_safety.execution_time_ms,

        g_last_safety.deadline_missed
            ? "true"
            : "false",

        g_last_safety.budget_overrun
            ? "true"
            : "false",

        g_have_control
            ? "true"
            : "false",

        (unsigned long long)
            g_last_control.cycle,

        (int)
            g_last_control.control_state,

        g_last_control.execution_time_ms,

        g_last_control.budget_ms,

        g_last_control.deadline_missed
            ? "true"
            : "false",

        g_last_control.budget_overrun
            ? "true"
            : "false",

        g_last_control.target_speed,

        g_last_control.throttle_percent,

        g_last_control.brake_percent
    );


    fclose(f);


    rename(
        DASHBOARD_STATUS_TMP_PATH,
        DASHBOARD_STATUS_PATH
    );
}


/* === Print MCTM decision === */

static void print_decision(void)
{
    printf(
        "[MCTM] decision | mode=%s "
        "safety_util=%.3f "
        "control_util=%.3f "
        "protect_safety=%d "
        "protect_control=%d "
        "allow_low=%d\n",

        mode_text(
            g_current_decision.mctm_mode
        ),

        g_current_decision.safety_utilization,

        g_current_decision.control_utilization,

        g_current_decision.protect_safety,

        g_current_decision.protect_control,

        g_current_decision.allow_low_criticality
    );

    fflush(stdout);
}


/* === Main === */

int main(void)
{
    name_attach_t *attach;


    mctm_dash_header("MCTM RESOURCE MONITOR", "Mixed-criticality policy engine");
    mctm_dash_row("Role", "budget + deadline monitor");
    mctm_dash_row("Priority", "20 (SCHED_FIFO)");
    mctm_dash_row("Policy", "NORMAL -> DEGRADED -> EMERGENCY");
    mctm_dash_row("IPC", "Safety + Control + Infotainment");
    mctm_dash_row("Snapshot", DASHBOARD_STATUS_PATH);
    mctm_dash_footer();

    struct sched_param param;

    memset(
        &param,
        0,
        sizeof(param)
    );


    param.sched_priority =
        RESOURCE_MONITOR_PRIORITY;


    if (
        pthread_setschedparam(
            pthread_self(),
            SCHED_FIFO,
            &param
        ) != 0
    )
    {
        perror(
            "[MCTM] pthread_setschedparam"
        );
    }


    attach =
        name_attach(
            NULL,
            RESOURCE_MONITOR_CHANNEL,
            0
        );


    if (attach == NULL)
    {
        perror(
            "[MCTM] name_attach"
        );

        return EXIT_FAILURE;
    }


    g_chid =
        attach->chid;


    memset(
        &g_current_decision,
        0,
        sizeof(g_current_decision)
    );


    g_current_decision.type =
        MCTM_MSG_MCTM_DECISION;

    g_current_decision.mctm_mode =
        MCTM_MODE_DEGRADED;

    g_current_decision.protect_safety =
        1;

    g_current_decision.protect_control =
        1;

    /*
     * Startup remains conservative until both
     * SafetyTask and ControlTask have reported.
     */
    g_current_decision.allow_low_criticality =
        0;

    memset(
        &g_last_dispatched_decision,
        0,
        sizeof(g_last_dispatched_decision)
    );

    g_have_dispatched_decision = 0;


    mctm_dash_row("Channel", RESOURCE_MONITOR_CHANNEL);
    mctm_dash_row("Startup mode", "DEGRADED (until peers report)");

    printf(
        "[MCTM] Waiting for task status...\n"
    );

    fflush(stdout);


    while (g_running)
    {
        union
        {
            SafetyStatusMessage safety;

            ControlStatusMessage control;

            MCTMGetDecisionRequest request;

            uint8_t raw[
                sizeof(SafetyStatusMessage)
            ];

        } message;


        struct _msg_info info;


        memset(
            &message,
            0,
            sizeof(message)
        );


        int rcvid =
            MsgReceive(
                attach->chid,
                &message,
                sizeof(message),
                &info
            );


        if (rcvid == -1)
        {
            if (errno == EINTR)
                continue;


            perror(
                "[MCTM] MsgReceive"
            );

            break;
        }


        /* === QNX pulse. === */
        if (rcvid == 0)
            continue;


        if (
            info.msglen <
            (int)sizeof(int)
        )
        {
            printf(
                "[MCTM] Invalid message length=%d\n",
                info.msglen
            );

            fflush(stdout);


            MsgError(
                rcvid,
                EINVAL
            );

            continue;
        }


        int message_type =
            message.request.type;


        switch (message_type)
        {
            /* === SafetyTask status === */

            case MCTM_MSG_SAFETY_STATUS:

                memcpy(
                    &g_last_safety,
                    &message.safety,
                    sizeof(g_last_safety)
                );

                g_have_safety =
                    1;


                evaluate_decision();

                print_decision();


                {
                    MCTMReply reply;

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
                            "[MCTM] SafetyStatus "
                            "MsgReply failed: %s\n",
                            strerror(errno)
                        );

                        fflush(stdout);
                    }
                }


                /* === Forward the consolidated decision/snapshot. === */
                /*
                 * MCTM policy is pushed only when the policy fields
                 * actually change. SafetyMonitor remains a continuous
                 * observer and receives every consolidated snapshot.
                 */
                {
                    int decision_changed = 0;

                    if (!g_have_dispatched_decision)
                    {
                        decision_changed = 1;
                    }
                    else if (decision_policy_changed(
                                 &g_last_dispatched_decision,
                                 &g_current_decision))
                    {
                        decision_changed = 1;
                    }

                    if (decision_changed)
                    {
                        printf(
                            "[MCTM] Decision changed: "
                            "mode=%s protect_safety=%d "
                            "protect_control=%d allow_low=%d\n",
                            mode_text(g_current_decision.mctm_mode),
                            g_current_decision.protect_safety,
                            g_current_decision.protect_control,
                            g_current_decision.allow_low_criticality
                        );
                        fflush(stdout);

                        push_decision_to_control();
                        push_decision_to_infotainment();

                        g_last_dispatched_decision =
                            g_current_decision;
                        g_have_dispatched_decision = 1;
                    }
                }

                push_snapshot_to_safetymon();

                write_dashboard_snapshot();

                push_snapshot_to_telemetry();

                break;


            /* === ControlTask status === */

            case MCTM_MSG_CONTROL_STATUS:

                memcpy(
                    &g_last_control,
                    &message.control,
                    sizeof(g_last_control)
                );

                g_have_control =
                    1;


                evaluate_decision();

                print_decision();


                {
                    MCTMReply reply;

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
                            "[MCTM] ControlStatus "
                            "MsgReply failed: %s\n",
                            strerror(errno)
                        );

                        fflush(stdout);
                    }
                }


                /*
                 * MCTM policy is pushed only when the policy fields
                 * actually change. SafetyMonitor remains a continuous
                 * observer and receives every consolidated snapshot.
                 */
                {
                    int decision_changed = 0;

                    if (!g_have_dispatched_decision)
                    {
                        decision_changed = 1;
                    }
                    else if (decision_policy_changed(
                                 &g_last_dispatched_decision,
                                 &g_current_decision))
                    {
                        decision_changed = 1;
                    }

                    if (decision_changed)
                    {
                        printf(
                            "[MCTM] Decision changed: "
                            "mode=%s protect_safety=%d "
                            "protect_control=%d allow_low=%d\n",
                            mode_text(g_current_decision.mctm_mode),
                            g_current_decision.protect_safety,
                            g_current_decision.protect_control,
                            g_current_decision.allow_low_criticality
                        );
                        fflush(stdout);

                        push_decision_to_control();
                        push_decision_to_infotainment();

                        g_last_dispatched_decision =
                            g_current_decision;
                        g_have_dispatched_decision = 1;
                    }
                }

                push_snapshot_to_safetymon();

                write_dashboard_snapshot();

                push_snapshot_to_telemetry();

                break;


            /* === Infotainment initial decision query === */

            case MCTM_MSG_GET_DECISION:

                evaluate_decision();


                MsgReply(
                    rcvid,
                    EOK,
                    &g_current_decision,
                    sizeof(g_current_decision)
                );

                break;


            default:

                printf(
                    "[MCTM] Unknown message type=%d\n",
                    message_type
                );

                MsgError(
                    rcvid,
                    ENOSYS
                );

                break;
        }
    }


    if (
        g_control_coid != -1
    )
    {
        name_close(
            g_control_coid
        );
    }


    if (
        g_safetymon_coid != -1
    )
    {
        name_close(
            g_safetymon_coid
        );
    }


    if (
        g_infotainment_coid != -1
    )
    {
        name_close(
            g_infotainment_coid
        );
    }

    if (
        g_telemetry_coid != -1
    )
    {
        name_close(
            g_telemetry_coid
        );
    }


    name_detach(
        attach,
        0
    );


    printf(
        "[MCTM] ResourceMonitor stopped\n"
    );


    return EXIT_SUCCESS;
}
