/*
 * SPDX-License-Identifier: MIT
 *
 * HyperSafe - SafetyTask
 *
 * Highest-criticality periodic safety task.
 *
 * Hardware inputs are owned by VehicleIO.
 * SafetyTask never maps BCM2711 GPIO.
 *
 * SafetyTask publishes SafetyStatusMessage to:
 *
 *     1. ControlTask
 *     2. ResourceMonitor
 *
 * ResourceMonitor is the ONLY component that forwards the
 * consolidated supervision information to SafetyMonitor.
 *
 * There is intentionally NO direct:
 *
 *     SafetyTask -> SafetyMonitor
 *
 * communication.
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
#include "mctm_types.h"
#include "mctm_messages.h"
#include "vehicle_io_msg.h"
#include "SchedulingLogger.h"
#include "fault_injector.h"
#include "fault_reader.h"


/* === QNX IPC / Task Configuration === */

#define VEHICLE_IO_CHANNEL       "vehicle_io"
#define SAFETY_CHANNEL           "safety_task_step1"
#define CONTROL_CHANNEL_NAME     "control_task"
#define RESOURCE_MONITOR_CHANNEL "resource_monitor"


/*
 * SafetyTask is the highest-criticality periodic task.
 *
 * Keep outbound IPC bounded so a stalled consumer cannot
 * consume the entire SafetyTask budget/deadline.
 */
#define SAFETY_IPC_SEND_TIMEOUT_NS \
    (5ULL * 1000000ULL)


#define SAFETY_PERIOD_MS   100

#define ULTRASONIC_WARNING_CM   100.0f
#define ULTRASONIC_CRITICAL_CM   30.0f

#define PULSE_STEP1 1


/* === Safety Filtering / Debouncing === */

#define ULTRASONIC_INVALID_LIMIT 3
#define IR_DETECTED_LIMIT        3


/* === Global QNX Objects === */

static int g_vio_coid = -1;
static int g_chid = -1;
static int g_coid = -1;

static volatile int g_running = 1;

static timer_t g_timerid;


/* =========================================================
 * MCTM peer connections
 *
 * SafetyStatusMessage is sent ONLY to:
 *
 *     ControlTask
 *     ResourceMonitor
 *
 * ResourceMonitor subsequently sends its consolidated
 * supervision snapshot to SafetyMonitor.
 * ========================================================= */

static int g_control_coid  = -1;
static int g_resource_coid = -1;


/* === Safety timing statistics === */

static uint64_t g_safety_deadline_misses = 0;
static uint64_t g_safety_budget_overruns = 0;


/* === Filter State === */

static int g_ultrasonic_invalid_count = 0;
static int g_ir_detected_count = 0;


/* === Safety Report === */

typedef struct
{
    SafetyState level;
    uint32_t cycle;
    uint64_t timestamp_ns;
    VehicleIoInputs vio;
    const char *reason;

} SafetyReport;


/* === Safety Text === */

static const char *safety_text(
        SafetyState level)
{
    switch (level)
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


/* =========================================================
 * Connect Required MCTM Peer
 *
 * SafetyTask waits for:
 *
 *     ControlTask
 *     ResourceMonitor
 *
 * Both are required for the safety/MCTM loop.
 * ========================================================= */

static int connect_required_peer(
        const char *name)
{
    int coid;

    for (;;)
    {
        coid =
            name_open(
                name,
                0
            );

        if (coid != -1)
        {
            printf(
                "[SAFETY] Connected to %s (coid=%d)\n",
                name,
                coid
            );

            fflush(stdout);

            return coid;
        }

        printf(
            "[SAFETY] Waiting for %s: %s\n",
            name,
            strerror(errno)
        );

        fflush(stdout);

        delay(250);
    }
}


/* === Bounded SafetyStatus MsgSend === */

static int send_status_with_timeout(
        int coid,
        const SafetyStatusMessage *msg,
        MCTMReply *reply)
{
    uint64_t timeout_ns =
        SAFETY_IPC_SEND_TIMEOUT_NS;


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
        sizeof(*msg),
        reply,
        sizeof(*reply)
    );
}


/* === Map Safety reason into canonical SafetyReason === */

static SafetyReason map_safety_reason(
        const char *reason)
{
    if (reason == NULL)
        return REASON_NONE;


    if (
        strcmp(
            reason,
            "ULTRASONIC_CRITICAL_DISTANCE"
        ) == 0
    )
        return REASON_CRITICAL_BRAKING;


    if (
        strcmp(
            reason,
            "ULTRASONIC_INVALID_HOLD_CRITICAL"
        ) == 0
    )
        return REASON_CRITICAL_BRAKING;


    if (
        strcmp(
            reason,
            "ULTRASONIC_WARNING_DISTANCE"
        ) == 0
    )
        return REASON_COLLISION_RISK;


    if (
        strcmp(
            reason,
            "IR_PROXIMITY"
        ) == 0
    )
        return REASON_COLLISION_RISK;


    if (
        strcmp(
            reason,
            "ULTRASONIC_INVALID"
        ) == 0
    )
        return REASON_BRAKE_ANOMALY;


    if (
        strcmp(
            reason,
            "IPC_FAILURE"
        ) == 0
    )
        return REASON_BRAKE_ANOMALY;


    if (
        strcmp(
            reason,
            "HIGH_TEMPERATURE_WARNING"
        ) == 0
    )
        return REASON_HIGH_TEMPERATURE;


    if (
        strcmp(
            reason,
            "HIGH_TEMPERATURE_CRITICAL"
        ) == 0
    )
        return REASON_HIGH_TEMPERATURE;


    return REASON_NONE;
}


/* =========================================================
 * Publish SafetyStatusMessage
 *
 * ONLY:
 *
 *     SafetyTask -> ControlTask
 *     SafetyTask -> ResourceMonitor
 *
 * There is NO SafetyMonitor send here.
 * ========================================================= */

static void publish_safety_status(
        const SafetyStatusMessage *msg)
{
    MCTMReply reply;


    /* === ControlTask === */

    memset(
        &reply,
        0,
        sizeof(reply)
    );


    if (
        send_status_with_timeout(
            g_control_coid,
            msg,
            &reply
        ) == -1
    )
    {
        printf(
            "[SAFETY-IPC] "
            "ControlTask send failed: %s\n",
            strerror(errno)
        );

        fflush(stdout);


        /* === Reconnect on next cycle. === */
        name_close(
            g_control_coid
        );

        g_control_coid = -1;
    }


    /* === ResourceMonitor === */

    memset(
        &reply,
        0,
        sizeof(reply)
    );


    if (
        send_status_with_timeout(
            g_resource_coid,
            msg,
            &reply
        ) == -1
    )
    {
        printf(
            "[SAFETY-IPC] "
            "ResourceMonitor send failed: %s\n",
            strerror(errno)
        );

        fflush(stdout);


        name_close(
            g_resource_coid
        );

        g_resource_coid = -1;
    }


    /*
     * IMPORTANT:
     *
     * Do NOT send to SafetyMonitor here.
     *
     * ResourceMonitor receives this message and creates
     * SafetyMonitorSnapshot for the independent observer.
     */
}


/* === Monotonic Time === */

static uint64_t monotonic_ns(void)
{
    struct timespec ts;


    if (
        clock_gettime(
            CLOCK_MONOTONIC,
            &ts
        ) == -1
    )
    {
        return 0;
    }


    return
        ((uint64_t)ts.tv_sec *
            1000000000ULL)
        +
        (uint64_t)ts.tv_nsec;
}


/* === Connect VehicleIO === */

static int connect_vehicle_io(void)
{
    int coid;


    for (;;)
    {
        coid =
            name_open(
                VEHICLE_IO_CHANNEL,
                0
            );


        if (coid != -1)
        {
            printf(
                "[SAFETY] Connected to VehicleIO "
                "(coid=%d)\n",
                coid
            );

            fflush(stdout);

            return coid;
        }


        printf(
            "[SAFETY] Waiting for VehicleIO: %s\n",
            strerror(errno)
        );

        fflush(stdout);


        delay(250);
    }
}


/* === Read VehicleIO inputs === */

static int read_vehicle_inputs(
        VehicleIoInputs *inputs)
{
    VehicleIoMsg msg;
    VehicleIoReply reply;


    if (inputs == NULL)
        return -1;


    memset(
        &msg,
        0,
        sizeof(msg)
    );

    memset(
        &reply,
        0,
        sizeof(reply)
    );


    msg.type =
        VIO_MSG_READ_INPUTS;


    if (
        MsgSend(
            g_vio_coid,
            &msg,
            sizeof(msg),
            &reply,
            sizeof(reply)
        ) == -1
    )
    {
        return -1;
    }


    if (reply.status != 0)
        return reply.status;


    *inputs =
        reply.inputs;


    return 0;
}


/* === Set VehicleIO Safety Mode === */

static int set_vehicle_io_mode(
        SafetyState level)
{
    VehicleIoMsg msg;
    VehicleIoReply reply;

    int mode;


    switch (level)
    {
        case SAFETY_NORMAL:
            mode = MCTM_MODE_NORMAL;
            break;


        case SAFETY_WARNING:
            mode = MCTM_MODE_DEGRADED;
            break;


        case SAFETY_CRITICAL:
        default:
            mode = MCTM_MODE_EMERGENCY;
            break;
    }


    memset(
        &msg,
        0,
        sizeof(msg)
    );

    memset(
        &reply,
        0,
        sizeof(reply)
    );


    msg.type =
        VIO_MSG_SET_MODE;

    msg.mode =
        mode;


    /* === No physical temperature sensor. === */
    msg.temp_valid = 0;


    if (
        MsgSend(
            g_vio_coid,
            &msg,
            sizeof(msg),
            &reply,
            sizeof(reply)
        ) == -1
    )
    {
        return -1;
    }


    return reply.status;
}


/* === Safety Evaluation === */

static SafetyState g_last_safety_level =
    SAFETY_NORMAL;


static SafetyState evaluate_safety(
        const VehicleIoInputs *in,
        const char **reason)
{
    if (reason != NULL)
        *reason = "NONE";


    if (in == NULL)
    {
        if (reason != NULL)
            *reason = "NO_INPUT";

        g_last_safety_level =
            SAFETY_CRITICAL;

        return SAFETY_CRITICAL;
    }


    /* === Ultrasonic INVALID === */

    if (!in->ultrasonic_valid)
    {
        g_ultrasonic_invalid_count++;


        /*
         * Never downgrade a previously critical condition
         * solely because the sensor becomes temporarily
         * invalid.
         */

        if (
            g_last_safety_level ==
            SAFETY_CRITICAL
        )
        {
            if (reason != NULL)
                *reason =
                    "ULTRASONIC_INVALID_HOLD_CRITICAL";

            return SAFETY_CRITICAL;
        }


        /* === First two invalid samples are transient. === */

        if (
            g_ultrasonic_invalid_count <
            ULTRASONIC_INVALID_LIMIT
        )
        {
            if (reason != NULL)
                *reason =
                    "ULTRASONIC_TRANSIENT_INVALID";

            return g_last_safety_level;
        }


        /* === Persistent invalid samples. === */

        if (reason != NULL)
            *reason =
                "ULTRASONIC_INVALID";


        g_last_safety_level =
            SAFETY_WARNING;

        return SAFETY_WARNING;
    }


    /* === Valid ultrasonic sample === */

    g_ultrasonic_invalid_count = 0;


    /* === CRITICAL === */

    if (
        in->obstacle_distance_cm <=
        ULTRASONIC_CRITICAL_CM
    )
    {
        if (reason != NULL)
            *reason =
                "ULTRASONIC_CRITICAL_DISTANCE";


        g_last_safety_level =
            SAFETY_CRITICAL;


        return SAFETY_CRITICAL;
    }


    /* === WARNING === */

    if (
        in->obstacle_distance_cm <=
        ULTRASONIC_WARNING_CM
    )
    {
        if (reason != NULL)
            *reason =
                "ULTRASONIC_WARNING_DISTANCE";


        g_last_safety_level =
            SAFETY_WARNING;


        return SAFETY_WARNING;
    }


    /* === IR debounce === */

    if (in->obstacle_ir)
    {
        g_ir_detected_count++;


        if (
            g_ir_detected_count >=
            IR_DETECTED_LIMIT
        )
        {
            if (reason != NULL)
                *reason =
                    "IR_PROXIMITY";


            g_last_safety_level =
                SAFETY_WARNING;


            return SAFETY_WARNING;
        }
    }
    else
    {
        g_ir_detected_count = 0;
    }


    /* === NORMAL === */

    if (reason != NULL)
        *reason = "NONE";


    g_last_safety_level =
        SAFETY_NORMAL;


    return SAFETY_NORMAL;
}


/* === FaultInjector integration === */

static FaultReader g_fault_reader;


/* === Periodic Timer === */

static int configure_periodic_timer(void)
{
    struct sigevent event;

    struct itimerspec spec;


    SIGEV_PULSE_INIT(
        &event,
        g_coid,
        SIGEV_PULSE_PRIO_INHERIT,
        PULSE_STEP1,
        0
    );


    if (
        timer_create(
            CLOCK_MONOTONIC,
            &event,
            &g_timerid
        ) == -1
    )
    {
        printf(
            "[SAFETY] timer_create failed: %s\n",
            strerror(errno)
        );

        return -1;
    }


    memset(
        &spec,
        0,
        sizeof(spec)
    );


    spec.it_value.tv_sec = 0;

    spec.it_value.tv_nsec =
        SAFETY_PERIOD_MS *
        1000000L;

    spec.it_interval =
        spec.it_value;


    if (
        timer_settime(
            g_timerid,
            0,
            &spec,
            NULL
        ) == -1
    )
    {
        printf(
            "[SAFETY] timer_settime failed: %s\n",
            strerror(errno)
        );


        timer_delete(
            g_timerid
        );


        return -1;
    }


    return 0;
}


/* === Execute one safety cycle === */

static void run_cycle(
        uint32_t cycle)
{
    VehicleIoInputs inputs;

    const char *reason =
        "IPC_FAILURE";

    SafetyState level =
        SAFETY_CRITICAL;


    uint64_t start_ns;
    uint64_t end_ns;


    FaultInjectionState fault;

    int fault_active;


    double report_temperature =
        0.0;

    double report_brake_pressure =
        0.0;


    memset(
        &inputs,
        0,
        sizeof(inputs)
    );

    memset(
        &fault,
        0,
        sizeof(fault)
    );


    /*
     * FaultInjector is optional.
     *
     * CPU_OVERLOAD is consumed by ControlTask and therefore
     * is intentionally ignored here.
     */

    fault_active =
        (fault_reader_read(
            &g_fault_reader,
            &fault
        ) == 0)
        &&
        fault.active
        &&
        (fault.fault_type !=
            FAULT_CLEAR)
        &&
        (fault.fault_type !=
            FAULT_CPU_OVERLOAD);


    /* === Start execution measurement === */

    start_ns =
        monotonic_ns();


    scheduling_log(
        start_ns,
        cycle,
        "SafetyTask",
        LOG_START,
        0.0,
        "PENDING",
        "PENDING",
        "PENDING",
        "HIGHEST"
    );


    /* === Read VehicleIO === */

    if (
        read_vehicle_inputs(
            &inputs
        ) == 0
    )
    {
        /* === Explicitly mark invalid ultrasonic input. === */

        if (
            !inputs.ultrasonic_valid
        )
        {
            inputs.obstacle_distance_cm =
                -1.0f;
        }


        /* === Software-only sensor fault injection. === */

        if (
            fault_active &&
            fault.fault_type ==
                FAULT_SENSOR_INVALID
        )
        {
            inputs.ultrasonic_valid =
                0;

            inputs.obstacle_distance_cm =
                -1.0f;
        }


        level =
            evaluate_safety(
                &inputs,
                &reason
            );
    }
    else
    {
        /* === VehicleIO failure is CRITICAL. === */

        inputs.ultrasonic_valid = 0;
        inputs.hall_valid = 0;
        inputs.obstacle_distance_cm = -1.0f;
        inputs.wheel_rpm = 0.0f;
        inputs.obstacle_ir = 0;
        inputs.brake_pressed = 0;

        reason =
            "IPC_FAILURE";

        level =
            SAFETY_CRITICAL;
    }


    /* === Update VehicleIO indicators === */

    if (
        set_vehicle_io_mode(
            level
        ) != 0
    )
    {
        printf(
            "[SAFETY] VehicleIO mode update failed: %s\n",
            strerror(errno)
        );
    }


    /* === End execution measurement === */

    end_ns =
        monotonic_ns();


    {
        double execution_time_ms =
            (double)(
                end_ns - start_ns
            ) /
            1000000.0;


        int deadline_missed =
            execution_time_ms >
            (double)SAFETY_DEADLINE_MS;


        int budget_overrun =
            execution_time_ms >
            (double)SAFETY_BUDGET_MS;


        SafetyStatusMessage status;


        if (deadline_missed)
            g_safety_deadline_misses++;


        if (budget_overrun)
            g_safety_budget_overruns++;


        /* === Build SafetyStatusMessage === */

        memset(
            &status,
            0,
            sizeof(status)
        );


        status.type =
            MCTM_MSG_SAFETY_STATUS;


        status.cycle =
            cycle;


        status.safety_state =
            level;


        status.safety_reason =
            map_safety_reason(
                reason
            );


        status.temperature =
            report_temperature;


        status.brake_pressure =
            report_brake_pressure;


        status.obstacle_distance =
            inputs.obstacle_distance_cm /
            100.0;


        status.vehicle_speed =
            inputs.wheel_rpm;


        status.brake_pressed =
            inputs.brake_pressed;


        status.execution_time_ms =
            execution_time_ms;


        status.deadline_missed =
            deadline_missed;


        status.budget_overrun =
            budget_overrun;


        /* === Scheduling trace === */

        scheduling_log(
            end_ns,
            cycle,
            "SafetyTask",
            LOG_END,
            execution_time_ms,
            deadline_missed
                ? "MISS"
                : "OK",
            budget_overrun
                ? "OVERRUN"
                : "OK",
            safety_text(level),
            "HIGHEST"
        );


        /*
         * -------------------------------------------------
         * Publish to ControlTask + ResourceMonitor
         *
         * NOT SafetyMonitor.
         * -------------------------------------------------
         */

        publish_safety_status(
            &status
        );


        /* === CLI output === */

        printf(
            "[SAFETY] cycle=%lu "
            "level=%s "
            "reason=%s "
            "distance=%.1fcm(%s) "
            "rpm=%.1f(%s) "
            "ir=%s "
            "brake=%s "
            "exec=%.3fms "
            "deadline=%s "
            "budget=%s "
            "ultra_invalid=%d "
            "ir_detect=%d\n",

            (unsigned long)cycle,

            safety_text(level),

            reason,

            inputs.obstacle_distance_cm,

            inputs.ultrasonic_valid
                ? "valid"
                : "invalid",

            inputs.wheel_rpm,

            inputs.hall_valid
                ? "valid"
                : "invalid",

            inputs.obstacle_ir
                ? "DETECTED"
                : "CLEAR",

            inputs.brake_pressed
                ? "PRESSED"
                : "RELEASED",

            execution_time_ms,

            deadline_missed
                ? "MISS"
                : "OK",

            budget_overrun
                ? "OVERRUN"
                : "OK",

            g_ultrasonic_invalid_count,

            g_ir_detected_count
        );


        printf(
            "          TotalDeadlineMisses=%llu "
            "TotalBudgetOverruns=%llu\n",

            (unsigned long long)
                g_safety_deadline_misses,

            (unsigned long long)
                g_safety_budget_overruns
        );
    }
}


/* === Main === */

int main(void)
{
    struct sched_param sp;

    struct _pulse pulse;

    uint32_t cycle = 0;


    /* === Real-time priority === */

    sp.sched_priority =
        SAFETY_TASK_PRIORITY;


    if (
        pthread_setschedparam(
            pthread_self(),
            SCHED_FIFO,
            &sp
        ) != 0
    )
    {
        printf(
            "[SAFETY] "
            "SCHED_FIFO setup failed: %s\n",
            strerror(errno)
        );


        printf(
            "[SAFETY] "
            "Run with the QNX scheduling privilege "
            "required by the target image.\n"
        );


        return EXIT_FAILURE;
    }


    /* === Scheduling logger === */

    scheduling_logger_init();

    fault_reader_init(
        &g_fault_reader
    );


    /* === VehicleIO === */

    g_vio_coid =
        connect_vehicle_io();


    /*
     * -----------------------------------------------------
     * MCTM peers
     *
     * IMPORTANT:
     *
     * No SafetyMonitor connection.
     * -----------------------------------------------------
     */

    g_control_coid =
        connect_required_peer(
            CONTROL_CHANNEL_NAME
        );


    g_resource_coid =
        connect_required_peer(
            RESOURCE_MONITOR_CHANNEL
        );


    /* === SafetyTask own channel === */

    g_chid =
        ChannelCreate(0);


    if (g_chid == -1)
    {
        printf(
            "[SAFETY] ChannelCreate failed: %s\n",
            strerror(errno)
        );


        name_close(
            g_vio_coid
        );


        return EXIT_FAILURE;
    }


    /* === Attach connection to own channel === */

    g_coid =
        ConnectAttach(
            0,
            0,
            g_chid,
            _NTO_SIDE_CHANNEL,
            0
        );


    if (g_coid == -1)
    {
        printf(
            "[SAFETY] ConnectAttach failed: %s\n",
            strerror(errno)
        );


        ChannelDestroy(
            g_chid
        );


        name_close(
            g_vio_coid
        );


        return EXIT_FAILURE;
    }


    /* === Periodic timer === */

    if (
        configure_periodic_timer() != 0
    )
    {
        ConnectDetach(
            g_coid
        );


        ChannelDestroy(
            g_chid
        );


        name_close(
            g_vio_coid
        );


        return EXIT_FAILURE;
    }


    {
        char value[48];

        mctm_dash_header("MCTM SAFETY TASK", "HIGHEST-criticality periodic safety loop");

        snprintf(value, sizeof(value), "%d (SCHED_FIFO)", SAFETY_TASK_PRIORITY);
        mctm_dash_row("Priority", value);

        snprintf(value, sizeof(value), "%d ms", SAFETY_PERIOD_MS);
        mctm_dash_row("Period", value);

        snprintf(value, sizeof(value), "%d / %d ms", SAFETY_BUDGET_MS, SAFETY_DEADLINE_MS);
        mctm_dash_row("Budget / deadline", value);

        mctm_dash_row("Inputs", "VehicleIO -> ultrasonic / Hall / IR");
        mctm_dash_row("Outputs", "VehicleIO safety mode + status IPC");
        mctm_dash_row("Peers", "ControlTask + ResourceMonitor");
        mctm_dash_row("Fault source", "/mctm_fault_injector");
        mctm_dash_footer();
    }


    /* === Main periodic message loop === */

    for (;;)
    {
        int rcvid;


        rcvid =
            MsgReceive(
                g_chid,
                &pulse,
                sizeof(pulse),
                NULL
            );


        /* === Timer pulse. === */

        if (rcvid == 0)
        {
            if (
                pulse.code ==
                PULSE_STEP1
            )
            {
                run_cycle(
                    cycle++
                );
            }

            continue;
        }


        /* === Receive error. === */

        if (rcvid == -1)
        {
            if (errno == EINTR)
                continue;


            break;
        }


        /* === Unexpected message. === */

        MsgError(
            rcvid,
            ENOTSUP
        );
    }


    /* === Shutdown === */

    g_running = 0;


    timer_delete(
        g_timerid
    );


    ConnectDetach(
        g_coid
    );


    ChannelDestroy(
        g_chid
    );


    name_close(
        g_vio_coid
    );


    if (
        g_control_coid != -1
    )
    {
        name_close(
            g_control_coid
        );
    }


    if (
        g_resource_coid != -1
    )
    {
        name_close(
            g_resource_coid
        );
    }


    scheduling_logger_close();


    return EXIT_SUCCESS;
}
