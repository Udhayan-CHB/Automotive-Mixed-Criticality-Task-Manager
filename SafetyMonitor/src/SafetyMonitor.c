/*
 * ============================================================
 * HyperSafe - SafetyMonitor
 * ============================================================
 *
 * Independent supervisory / diagnostic layer.
 *
 * Responsibilities:
 *   - Receive supervision snapshots from ResourceMonitor
 *   - Monitor SafetyTask / ControlTask health
 *   - Detect stale status
 *   - Detect cycle regressions
 *   - Detect timing inconsistencies
 *   - Track emergency / warning events
 *
 * IMPORTANT:
 *   SafetyMonitor is NOT the MCTM decision maker.
 *   ResourceMonitor remains the decision authority.
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>

#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include "mctm_dashboard.h"

#include "mctm_config.h"
#include "mctm_types.h"
#include "mctm_messages.h"
#include "safety_monitor_msg.h"


/* === CONFIGURATION === */

#define SAFETY_MONITOR_CHANNEL       "SafetyMonitor"

#define SAFETY_STATUS_STALE_MS       300ULL
#define CONTROL_STATUS_STALE_MS      200ULL
#define RM_SNAPSHOT_STALE_MS         300ULL

#define SUPERVISOR_PERIOD_MS         100ULL

#define SAFETY_MONITOR_PRIORITY      15


/* === GLOBAL STATE === */

static volatile int g_running = 1;

static pthread_t g_supervisor_thread;
static int g_supervisor_thread_started = 0;

static pthread_mutex_t g_state_lock =
    PTHREAD_MUTEX_INITIALIZER;


/* === Latest snapshot received from ResourceMonitor. === */
static SafetyMonitorSnapshot g_snapshot;

static int g_have_snapshot = 0;


/* === Timestamp when latest snapshot was received. === */
static uint64_t g_last_snapshot_rx_ns = 0;


/* === Timestamp when SafetyTask last advanced. === */
static uint64_t g_last_safety_update_ns = 0;


/* === Timestamp when ControlTask last advanced. === */
static uint64_t g_last_control_update_ns = 0;


/* === Last observed cycles. === */
static uint64_t g_last_safety_cycle = 0;
static uint64_t g_last_control_cycle = 0;


/* === Have we seen a valid cycle yet? === */
static int g_seen_safety_cycle = 0;
static int g_seen_control_cycle = 0;


/* === Prevent repeated messages. === */
static int g_safety_stale_reported = 0;
static int g_control_stale_reported = 0;
static int g_rm_stale_reported = 0;

static int g_missing_safety_reported = 0;
static int g_missing_control_reported = 0;


/* === Statistics. === */
static uint64_t g_emergency_events = 0;
static uint64_t g_warning_events = 0;
static uint64_t g_inconsistency_events = 0;


/* === TIME === */

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
        return 0;

    return ((uint64_t)ts.tv_sec * 1000000000ULL) +
           (uint64_t)ts.tv_nsec;
}


/* === TEXT HELPERS === */

static const char *safety_text(SafetyState state)
{
    switch (state)
    {
        case SAFETY_NORMAL:
            return "NORMAL";

        case SAFETY_WARNING:
            return "WARNING";

        case SAFETY_CRITICAL:
            return "CRITICAL";

        default:
            return "UNKNOWN";
    }
}


static const char *control_text(ControlState state)
{
    switch (state)
    {
        case CONTROL_NORMAL:
            return "NORMAL";

        case CONTROL_CAUTION:
            return "CAUTION";

        case CONTROL_EMERGENCY:
            return "EMERGENCY";

        default:
            return "UNKNOWN";
    }
}


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


/* === EVENT LOGGER === */

static void log_event(
        const char *level,
        const char *text)
{
    printf(
        "[SAFETY-MONITOR][%s] %s\n",
        level,
        text
    );

    fflush(stdout);
}


/* === SNAPSHOT VALIDATION === */

static void inspect_snapshot(
        const SafetyMonitorSnapshot *s,
        uint64_t previous_safety_cycle,
        int had_previous_safety,
        uint64_t previous_control_cycle,
        int had_previous_control)
{
    int inconsistent = 0;

    if (s == NULL)
        return;


    /* === Cycle regression === */

    if (s->have_safety &&
        had_previous_safety &&
        s->safety_cycle < previous_safety_cycle)
    {
        inconsistent = 1;

        printf(
            "[SAFETY-MONITOR][ABNORMAL] "
            "SafetyTask cycle regression: "
            "%llu -> %llu\n",
            (unsigned long long)previous_safety_cycle,
            (unsigned long long)s->safety_cycle
        );
    }


    if (s->have_control &&
        had_previous_control &&
        s->control_cycle < previous_control_cycle)
    {
        inconsistent = 1;

        printf(
            "[SAFETY-MONITOR][ABNORMAL] "
            "ControlTask cycle regression: "
            "%llu -> %llu\n",
            (unsigned long long)previous_control_cycle,
            (unsigned long long)s->control_cycle
        );
    }


    /* === Invalid execution times === */

    if (s->safety_execution_ms < 0.0 ||
        s->control_execution_ms < 0.0)
    {
        inconsistent = 1;
    }


    /* === Execution time inconsistent with deadline status === */

    if (s->have_safety &&
        s->safety_execution_ms >
            (double)SAFETY_DEADLINE_MS &&
        !s->safety_deadline_missed)
    {
        inconsistent = 1;
    }


    if (s->have_control &&
        s->control_execution_ms >
            (double)CONTROL_DEADLINE_MS &&
        !s->control_deadline_missed)
    {
        inconsistent = 1;
    }


    /* === Report inconsistency === */

    if (inconsistent)
    {
        g_inconsistency_events++;

        printf(
            "[SAFETY-MONITOR][ABNORMAL] "
            "Inconsistent snapshot | "
            "SafetyCycle=%llu "
            "ControlCycle=%llu "
            "SafetyExec=%.3f ms "
            "ControlExec=%.3f ms "
            "SDeadline=%d "
            "CDeadline=%d\n",

            (unsigned long long)s->safety_cycle,
            (unsigned long long)s->control_cycle,

            s->safety_execution_ms,
            s->control_execution_ms,

            s->safety_deadline_missed,
            s->control_deadline_missed
        );

        fflush(stdout);
    }


    /* === Emergency monitoring === */

    if (s->safety_state == SAFETY_CRITICAL ||
        s->control_state == CONTROL_EMERGENCY ||
        s->mctm_mode == MCTM_MODE_EMERGENCY)
    {
        if (!had_previous_safety ||
            s->safety_cycle != previous_safety_cycle)
        {
            g_emergency_events++;

            printf(
                "[SAFETY-MONITOR][EMERGENCY] "
                "MCTM=%s | Safety=%s | Control=%s | "
                "SafetyCycle=%llu | ControlCycle=%llu\n",

                mode_text(s->mctm_mode),

                safety_text(s->safety_state),

                control_text(s->control_state),

                (unsigned long long)s->safety_cycle,

                (unsigned long long)s->control_cycle
            );

            fflush(stdout);
        }
    }


    /* === Warning / degraded monitoring === */

    else if (s->safety_state == SAFETY_WARNING ||
             s->control_state == CONTROL_CAUTION ||
             s->mctm_mode == MCTM_MODE_DEGRADED)
    {
        if (!had_previous_safety ||
            s->safety_cycle != previous_safety_cycle)
        {
            g_warning_events++;

            printf(
                "[SAFETY-MONITOR][WARNING] "
                "MCTM=%s | Safety=%s | Control=%s | "
                "SafetyCycle=%llu | ControlCycle=%llu\n",

                mode_text(s->mctm_mode),

                safety_text(s->safety_state),

                control_text(s->control_state),

                (unsigned long long)s->safety_cycle,

                (unsigned long long)s->control_cycle
            );

            fflush(stdout);
        }
    }


    /* === Timing anomalies === */

    if (s->safety_deadline_missed ||
        s->control_deadline_missed ||
        s->safety_budget_overrun ||
        s->control_budget_overrun)
    {
        printf(
            "[SAFETY-MONITOR][TIMING] "
            "Safety(deadline=%d budget=%d) "
            "Control(deadline=%d budget=%d)\n",

            s->safety_deadline_missed,
            s->safety_budget_overrun,

            s->control_deadline_missed,
            s->control_budget_overrun
        );

        fflush(stdout);
    }
}


/* === SUPERVISOR THREAD === */

static void *supervisor_thread(void *arg)
{
    (void)arg;

    struct sched_param param;

    memset(&param, 0, sizeof(param));

    param.sched_priority =
        SAFETY_MONITOR_PRIORITY;

    /* === Best effort priority setting. === */
    (void)pthread_setschedparam(
        pthread_self(),
        SCHED_FIFO,
        &param
    );


    while (g_running)
    {
        uint64_t now_ns;

        uint64_t last_snapshot_ns;
        uint64_t last_safety_ns;
        uint64_t last_control_ns;

        int have_snapshot;
        int have_safety;
        int have_control;


        now_ns = monotonic_ns();


        /* === Copy state under mutex. === */
        pthread_mutex_lock(&g_state_lock);

        last_snapshot_ns =
            g_last_snapshot_rx_ns;

        last_safety_ns =
            g_last_safety_update_ns;

        last_control_ns =
            g_last_control_update_ns;

        have_snapshot =
            g_have_snapshot;

        have_safety =
            g_snapshot.have_safety;

        have_control =
            g_snapshot.have_control;

        pthread_mutex_unlock(&g_state_lock);


        /* === ResourceMonitor availability === */

        if (!have_snapshot)
        {
            if (!g_rm_stale_reported)
            {
                log_event(
                    "WAITING",
                    "No ResourceMonitor snapshot received yet"
                );

                g_rm_stale_reported = 1;
            }
        }
        else
        {
            uint64_t age_ms;

            age_ms =
                (now_ns > last_snapshot_ns)
                    ? (now_ns - last_snapshot_ns) /
                        1000000ULL
                    : 0ULL;


            if (age_ms > RM_SNAPSHOT_STALE_MS)
            {
                if (!g_rm_stale_reported)
                {
                    log_event(
                        "STALE",
                        "ResourceMonitor snapshot is stale"
                    );

                    g_rm_stale_reported = 1;
                }
            }
            else
            {
                if (g_rm_stale_reported)
                {
                    log_event(
                        "RECOVERY",
                        "ResourceMonitor snapshot recovered"
                    );

                    g_rm_stale_reported = 0;
                }
            }


            /* === SafetyTask availability === */

            if (!have_safety)
            {
                if (!g_missing_safety_reported)
                {
                    log_event(
                        "MISSING",
                        "SafetyTask status has not been received"
                    );

                    g_missing_safety_reported = 1;
                }
            }
            else
            {
                if (g_missing_safety_reported)
                {
                    log_event(
                        "RECOVERY",
                        "SafetyTask status is now available"
                    );

                    g_missing_safety_reported = 0;
                }
            }


            /* === ControlTask availability === */

            if (!have_control)
            {
                if (!g_missing_control_reported)
                {
                    log_event(
                        "MISSING",
                        "ControlTask status has not been received"
                    );

                    g_missing_control_reported = 1;
                }
            }
            else
            {
                if (g_missing_control_reported)
                {
                    log_event(
                        "RECOVERY",
                        "ControlTask status is now available"
                    );

                    g_missing_control_reported = 0;
                }
            }


            /* === SafetyTask stale detection === */

            if (have_safety &&
                last_safety_ns != 0)
            {
                uint64_t age_ms;

                age_ms =
                    (now_ns > last_safety_ns)
                        ? (now_ns - last_safety_ns) /
                            1000000ULL
                        : 0ULL;


                if (age_ms >
                    SAFETY_STATUS_STALE_MS)
                {
                    if (!g_safety_stale_reported)
                    {
                        log_event(
                            "STALE",
                            "SafetyTask status is stale/missing"
                        );

                        g_safety_stale_reported = 1;
                    }
                }
                else
                {
                    if (g_safety_stale_reported)
                    {
                        log_event(
                            "RECOVERY",
                            "SafetyTask status recovered"
                        );

                        g_safety_stale_reported = 0;
                    }
                }
            }


            /* === ControlTask stale detection === */

            if (have_control &&
                last_control_ns != 0)
            {
                uint64_t age_ms;

                age_ms =
                    (now_ns > last_control_ns)
                        ? (now_ns - last_control_ns) /
                            1000000ULL
                        : 0ULL;


                if (age_ms >
                    CONTROL_STATUS_STALE_MS)
                {
                    if (!g_control_stale_reported)
                    {
                        log_event(
                            "STALE",
                            "ControlTask status is stale/missing"
                        );

                        g_control_stale_reported = 1;
                    }
                }
                else
                {
                    if (g_control_stale_reported)
                    {
                        log_event(
                            "RECOVERY",
                            "ControlTask status recovered"
                        );

                        g_control_stale_reported = 0;
                    }
                }
            }
        }


        /* === Supervisor period. === */
        {
            struct timespec sleep_ts;

            sleep_ts.tv_sec = 0;

            sleep_ts.tv_nsec =
                (long)(
                    SUPERVISOR_PERIOD_MS *
                    1000000ULL
                );

            nanosleep(
                &sleep_ts,
                NULL
            );
        }
    }

    return NULL;
}


/* === MAIN === */

int main(void)
{
    name_attach_t *attach;


    {
        char value[40];

        mctm_dash_header("HYPERSAFE SAFETY MONITOR", "Independent supervisory layer");

        snprintf(value, sizeof(value), "%d (SCHED_FIFO)", SAFETY_MONITOR_PRIORITY);
        mctm_dash_row("Priority", value);

        snprintf(value, sizeof(value), "%llu ms", (unsigned long long)SUPERVISOR_PERIOD_MS);
        mctm_dash_row("Supervisor period", value);

        snprintf(value, sizeof(value), "%llu ms", (unsigned long long)SAFETY_STATUS_STALE_MS);
        mctm_dash_row("Safety stale", value);

        snprintf(value, sizeof(value), "%llu ms", (unsigned long long)CONTROL_STATUS_STALE_MS);
        mctm_dash_row("Control stale", value);

        snprintf(value, sizeof(value), "%llu ms", (unsigned long long)RM_SNAPSHOT_STALE_MS);
        mctm_dash_row("RM snapshot stale", value);

        mctm_dash_row("IPC source", "resource_monitor");
        mctm_dash_footer();
    }


    /* === Create SafetyMonitor IPC channel === */

    attach =
        name_attach(
            NULL,
            SAFETY_MONITOR_CHANNEL,
            0
        );


    if (attach == NULL)
    {
        printf(
            "[SAFETY-MONITOR] "
            "name_attach(%s) failed: %s\n",

            SAFETY_MONITOR_CHANNEL,
            strerror(errno)
        );

        return EXIT_FAILURE;
    }


    printf(
        "[SAFETY-MONITOR] Channel ready: %s\n",
        SAFETY_MONITOR_CHANNEL
    );

    fflush(stdout);


    /* === Initialize state === */

    memset(
        &g_snapshot,
        0,
        sizeof(g_snapshot)
    );


    /* === Start supervisor thread === */

    if (pthread_create(
            &g_supervisor_thread,
            NULL,
            supervisor_thread,
            NULL) != 0)
    {
        printf(
            "[SAFETY-MONITOR] "
            "Failed to create supervisor thread\n"
        );

        name_detach(
            attach,
            0
        );

        return EXIT_FAILURE;
    }


    g_supervisor_thread_started = 1;


    /* === IPC receive loop === */

    while (g_running)
    {
        SafetyMonitorSnapshot snapshot;

        SafetyMonitorReply reply;

        struct _msg_info info;

        int rcvid;


        memset(
            &snapshot,
            0,
            sizeof(snapshot)
        );

        memset(
            &info,
            0,
            sizeof(info)
        );


        rcvid =
            MsgReceive(
                attach->chid,
                &snapshot,
                sizeof(snapshot),
                &info
            );


        /* === Receive error. === */
        if (rcvid == -1)
        {
            if (errno == EINTR)
                continue;

            printf(
                "[SAFETY-MONITOR] "
                "MsgReceive failed: %s\n",
                strerror(errno)
            );

            fflush(stdout);

            continue;
        }


        /* === Pulse. === */
        if (rcvid == 0)
        {
            continue;
        }


        /*
         * ----------------------------------------------------
         * Validate message size
         * ----------------------------------------------------
         *
         * This is an important addition.
         *
         * If ResourceMonitor and SafetyMonitor were compiled
         * with different versions of safety_monitor_msg.h,
         * the structures may have different sizes.
         */

        if (info.msglen !=
            (int)sizeof(SafetyMonitorSnapshot))
        {
            printf(
                "[SAFETY-MONITOR][ABI ERROR] "
                "Received message size=%d, "
                "expected=%llu\n",

                info.msglen,

                (unsigned long long)
                    sizeof(SafetyMonitorSnapshot)
            );

            fflush(stdout);

            MsgError(
                rcvid,
                EINVAL
            );

            continue;
        }


        /* === Validate message type === */

        if (snapshot.type !=
            SAFETYMON_MSG_SNAPSHOT)
        {
            printf(
                "[SAFETY-MONITOR] "
                "Unknown message type=%d\n",
                snapshot.type
            );

            fflush(stdout);

            MsgError(
                rcvid,
                ENOSYS
            );

            continue;
        }


        /* === Validate boolean/flag fields === */

        if (snapshot.have_safety != 0 &&
            snapshot.have_safety != 1)
        {
            printf(
                "[SAFETY-MONITOR][ABI ERROR] "
                "Invalid have_safety=%d\n",
                snapshot.have_safety
            );

            fflush(stdout);
            MsgError(rcvid, EINVAL);
            continue;
        }

        if (snapshot.have_control != 0 &&
            snapshot.have_control != 1)
        {
            printf(
                "[SAFETY-MONITOR][ABI ERROR] "
                "Invalid have_control=%d\n",
                snapshot.have_control
            );

            fflush(stdout);
            MsgError(rcvid, EINVAL);
            continue;
        }

        /* === Validate enum ranges === */

        if (snapshot.safety_state < SAFETY_NORMAL ||
            snapshot.safety_state > SAFETY_CRITICAL)
        {
            printf(
                "[SAFETY-MONITOR][ABI ERROR] "
                "Invalid SafetyState=%d\n",
                snapshot.safety_state
            );

            fflush(stdout);
            MsgError(rcvid, EINVAL);
            continue;
        }

        if (snapshot.control_state < CONTROL_NORMAL ||
            snapshot.control_state > CONTROL_EMERGENCY)
        {
            printf(
                "[SAFETY-MONITOR][ABI ERROR] "
                "Invalid ControlState=%d\n",
                snapshot.control_state
            );

            fflush(stdout);
            MsgError(rcvid, EINVAL);
            continue;
        }

        if (snapshot.mctm_mode < MCTM_MODE_NORMAL ||
            snapshot.mctm_mode > MCTM_MODE_EMERGENCY)
        {
            printf(
                "[SAFETY-MONITOR][ABI ERROR] "
                "Invalid MCTM mode=%d\n",
                snapshot.mctm_mode
            );

            fflush(stdout);
            MsgError(rcvid, EINVAL);
            continue;
        }

        /* === Process snapshot === */

        {
            uint64_t now_ns;

            uint64_t previous_safety_cycle = 0;
            uint64_t previous_control_cycle = 0;

            int had_previous_safety = 0;
            int had_previous_control = 0;


            now_ns =
                monotonic_ns();


            /* === Read previous state. === */
            pthread_mutex_lock(
                &g_state_lock
            );


            if (snapshot.have_safety)
            {
                previous_safety_cycle =
                    g_last_safety_cycle;

                had_previous_safety =
                    g_seen_safety_cycle;
            }


            if (snapshot.have_control)
            {
                previous_control_cycle =
                    g_last_control_cycle;

                had_previous_control =
                    g_seen_control_cycle;
            }


            /* === Store complete snapshot. === */
            g_snapshot =
                snapshot;

            g_have_snapshot = 1;

            g_last_snapshot_rx_ns =
                now_ns;


            /* === Update SafetyTask tracking. === */
            if (snapshot.have_safety)
            {
                if (!g_seen_safety_cycle ||
                    snapshot.safety_cycle >
                        g_last_safety_cycle)
                {
                    g_last_safety_update_ns =
                        now_ns;
                }


                if (!g_seen_safety_cycle ||
                    snapshot.safety_cycle >=
                        g_last_safety_cycle)
                {
                    g_last_safety_cycle =
                        snapshot.safety_cycle;
                }


                g_seen_safety_cycle = 1;
            }


            /* === Update ControlTask tracking. === */
            if (snapshot.have_control)
            {
                if (!g_seen_control_cycle ||
                    snapshot.control_cycle >
                        g_last_control_cycle)
                {
                    g_last_control_update_ns =
                        now_ns;
                }


                if (!g_seen_control_cycle ||
                    snapshot.control_cycle >=
                        g_last_control_cycle)
                {
                    g_last_control_cycle =
                        snapshot.control_cycle;
                }


                g_seen_control_cycle = 1;
            }


            pthread_mutex_unlock(
                &g_state_lock
            );


            /* === Inspect snapshot outside mutex. === */
            inspect_snapshot(
                &snapshot,

                previous_safety_cycle,
                had_previous_safety,

                previous_control_cycle,
                had_previous_control
            );
        }


        /* === Reply to ResourceMonitor === */

        memset(
            &reply,
            0,
            sizeof(reply)
        );

        reply.status = 0;


        if (MsgReply(
                rcvid,
                EOK,
                &reply,
                sizeof(reply)) == -1)
        {
            printf(
                "[SAFETY-MONITOR] "
                "MsgReply failed: %s\n",
                strerror(errno)
            );

            fflush(stdout);
        }
    }


    /* === Shutdown === */

    g_running = 0;


    if (g_supervisor_thread_started)
    {
        pthread_join(
            g_supervisor_thread,
            NULL
        );
    }


    name_detach(
        attach,
        0
    );


    printf(
        "[SAFETY-MONITOR] stopped | "
        "EmergencyEvents=%llu | "
        "WarningEvents=%llu | "
        "InconsistencyEvents=%llu\n",

        (unsigned long long)
            g_emergency_events,

        (unsigned long long)
            g_warning_events,

        (unsigned long long)
            g_inconsistency_events
    );


    return EXIT_SUCCESS;
}
