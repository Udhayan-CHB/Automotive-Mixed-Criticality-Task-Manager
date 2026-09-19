#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include "mctm_dashboard.h"

#include "SchedulingLogger.h"

static FILE *log_file = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void scheduling_logger_init(void)
{
    pthread_mutex_lock(&log_mutex);

    if (log_file == NULL)
    {
        FILE *check;
        int need_header = 1;

        /*
         * Append mode is required: SafetyTask, ControlTask and
         * ResourceMonitor all write to the same shared trace file.
         * Opening with "w" here would truncate whatever another
         * process had already logged (or will log) to the same
         * path -- exactly the kind of silent data loss the project's
         * own consistency rules warn against.
         */
        check = fopen("/tmp/schedule_trace.csv", "r");
        if (check != NULL)
        {
            fseek(check, 0, SEEK_END);
            if (ftell(check) > 0)
                need_header = 0;
            fclose(check);
        }

        log_file = fopen("/tmp/schedule_trace.csv", "a");

        if (log_file != NULL)
        {
            printf("[LOGGER] Opened /tmp/schedule_trace.csv successfully\n");

            if (need_header)
            {
                fprintf(
                    log_file,
                    "timestamp_ns,cycle,task,event,"
                    "execution_ms,deadline,budget,mode,criticality\n"
                );

                fflush(log_file);
            }
        }
        else
        {
            printf("[LOGGER] ERROR: Cannot open /tmp/schedule_trace.csv\n");
        }
    }

    pthread_mutex_unlock(&log_mutex);
}


void scheduling_log(
    uint64_t timestamp_ns,
    uint64_t cycle,
    const char *task,
    LogEvent event,
    double execution_ms,
    const char *deadline,
    const char *budget,
    const char *mode,
    const char *criticality)
{
    const char *event_text;

    switch (event)
    {
        case LOG_START:
            event_text = "START";
            break;

        case LOG_END:
            event_text = "END";
            break;

        case LOG_MODE_CHANGE:
            event_text = "MODE_CHANGE";
            break;

        case LOG_FAULT:
            event_text = "FAULT";
            break;

        default:
            event_text = "UNKNOWN";
            break;
    }

    pthread_mutex_lock(&log_mutex);

    if (log_file != NULL)
    {
        fprintf(
            log_file,
            "%llu,%llu,%s,%s,%.3f,%s,%s,%s,%s\n",
            (unsigned long long)timestamp_ns,
            (unsigned long long)cycle,
            task,
            event_text,
            execution_ms,
            deadline,
            budget,
            mode,
            criticality
        );

        fflush(log_file);
    }

    pthread_mutex_unlock(&log_mutex);
}


void scheduling_logger_close(void)
{
    pthread_mutex_lock(&log_mutex);

    if (log_file != NULL)
    {
        fclose(log_file);
        log_file = NULL;
    }

    pthread_mutex_unlock(&log_mutex);
}
