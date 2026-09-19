/*
 * MCTM Global Scheduling Trace Viewer
 *
 * Corrected behavior:
 *  - Sorts all records by timestamp for a true global task timeline.
 *  - Reports per-task execution/deadline/budget statistics.
 *  - Reports mode COUNTS from the trace.
 *  - Does NOT infer global mode transitions from individual task records.
 *
 * Usage:
 *   TraceViewer /dev/shmem/schedule_trace.csv
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mctm_dashboard.h"

#define MAX_RECORDS 20000
#define TASK_LEN 64
#define EVENT_LEN 16
#define MODE_LEN 16
#define CRIT_LEN 16

typedef struct {
    long long timestamp_ns;
    int cycle;
    char task[TASK_LEN];
    char event[EVENT_LEN];
    double execution_ms;
    char deadline[MODE_LEN];
    char budget[MODE_LEN];
    char mode[MODE_LEN];
    char criticality[CRIT_LEN];
} TraceRecord;

typedef struct {
    const char *name;
    const char *criticality;
    int completed;
    double total_exec;
    double max_exec;
    int deadline_misses;
    int budget_overruns;
} TaskStats;

static TraceRecord records[MAX_RECORDS];
static int record_count = 0;

static TaskStats stats[] = {
    {"SafetyTask", "HIGHEST", 0, 0.0, 0.0, 0, 0},
    {"ControlTask", "HIGH",    0, 0.0, 0.0, 0, 0},
    {"InfotainmentTask", "LOW", 0, 0.0, 0.0, 0, 0}
};

static int stats_count = sizeof(stats) / sizeof(stats[0]);

static int cmp_timestamp(const void *a, const void *b)
{
    const TraceRecord *ra = (const TraceRecord *)a;
    const TraceRecord *rb = (const TraceRecord *)b;

    if (ra->timestamp_ns < rb->timestamp_ns) return -1;
    if (ra->timestamp_ns > rb->timestamp_ns) return 1;

    /* Stable-ish tie break: START before END when timestamps are equal. */
    return strcmp(ra->event, rb->event);
}

static TaskStats *find_stats(const char *task)
{
    int i;
    for (i = 0; i < stats_count; ++i) {
        if (strcmp(stats[i].name, task) == 0)
            return &stats[i];
    }
    return NULL;
}

static int is_ok(const char *s)
{
    return strcmp(s, "OK") == 0;
}

static int is_mode(const char *s, const char *mode)
{
    return strcmp(s, mode) == 0;
}

static int load_csv(const char *path)
{
    FILE *fp;
    char line[512];

    fp = fopen(path, "r");
    if (!fp) {
        perror("fopen");
        return -1;
    }

    /* Skip CSV header. */
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -1;
    }

    while (fgets(line, sizeof(line), fp) && record_count < MAX_RECORDS) {
        TraceRecord *r = &records[record_count];

        if (sscanf(line,
                   "%lld,%d,%63[^,],%15[^,],%lf,%15[^,],%15[^,],%15[^,],%15[^\n]",
                   &r->timestamp_ns,
                   &r->cycle,
                   r->task,
                   r->event,
                   &r->execution_ms,
                   r->deadline,
                   r->budget,
                   r->mode,
                   r->criticality) == 9) {
            record_count++;
        }
    }

    fclose(fp);
    return 0;
}

static void calculate_stats(void)
{
    int i;

    for (i = 0; i < record_count; ++i) {
        TraceRecord *r = &records[i];
        TaskStats *s = find_stats(r->task);

        if (!s)
            continue;

        if (strcmp(r->event, "END") == 0) {
            s->completed++;
            s->total_exec += r->execution_ms;

            if (r->execution_ms > s->max_exec)
                s->max_exec = r->execution_ms;

            if (!is_ok(r->deadline))
                s->deadline_misses++;

            if (!is_ok(r->budget))
                s->budget_overruns++;
        }
    }
}

static void print_timeline(void)
{
    int i;

    printf("\n");
    mctm_dash_header("MCTM TRACE VIEWER", "CSV scheduling trace summary");
    printf("TIME(ms)     CYCLE   TASK                 EVENT        EXEC(ms)   DEADLINE   BUDGET     MODE         CRITICALITY\n");
    printf("--------------------------------------------------------------------------------\n");

    for (i = 0; i < record_count; ++i) {
        TraceRecord *r = &records[i];

        printf("%-12.3f %-7d %-20s %-12s %-10.3f %-10s %-10s %-12s %-12s\n",
               r->timestamp_ns / 1000000.0,
               r->cycle,
               r->task,
               r->event,
               r->execution_ms,
               r->deadline,
               r->budget,
               r->mode,
               r->criticality);
    }
}

static void print_summary(void)
{
    int i;
    int completed_total = 0;
    int invalid = 0;
    int normal = 0, degraded = 0, emergency = 0;

    for (i = 0; i < record_count; ++i) {
        TraceRecord *r = &records[i];

        if (strcmp(r->event, "END") == 0)
            completed_total++;

        if (is_mode(r->mode, "NORMAL"))
            normal++;
        else if (is_mode(r->mode, "DEGRADED"))
            degraded++;
        else if (is_mode(r->mode, "EMERGENCY"))
            emergency++;
    }

    printf("\n");
    mctm_dash_header("TASK SUMMARY", "Completed work and timing checks");
    printf("Valid CSV records     : %d\n", record_count);
    printf("Completed executions  : %d\n", completed_total);
    printf("Invalid records       : %d\n\n", invalid);

    for (i = 0; i < stats_count; ++i) {
        double avg = 0.0;
        if (stats[i].completed > 0)
            avg = stats[i].total_exec / stats[i].completed;

        printf("%s (%s)\n", stats[i].name, stats[i].criticality);
        printf("  Completed executions : %d\n", stats[i].completed);
        printf("  Average execution    : %.3f ms\n", avg);
        printf("  Maximum execution    : %.3f ms\n", stats[i].max_exec);
        printf("  Deadline misses      : %d\n", stats[i].deadline_misses);
        printf("  Budget overruns      : %d\n\n", stats[i].budget_overruns);
    }

    printf("\n");
    mctm_dash_header("MCTM MODE SUMMARY", "Logged task records");
    printf("NORMAL records        : %d\n", normal);
    printf("DEGRADED records      : %d\n", degraded);
    printf("EMERGENCY records     : %d\n", emergency);

    /*
     * IMPORTANT:
     * Do not print inferred global mode transitions here.
     *
     * The trace's MODE field belongs to each logged task record. Different
     * tasks can legitimately carry different snapshots around a transition.
     * Therefore, comparing adjacent records would manufacture false
     * EMERGENCY -> NORMAL -> EMERGENCY transitions.
     */
    printf("\n");
    printf("Mode transition note:\n");
    printf("  Global mode transitions are NOT inferred from per-task records.\n");
    printf("  Use the ResourceMonitor/MCTM decision log as the authoritative\n");
    printf("  source for NORMAL -> DEGRADED -> EMERGENCY transitions.\n");
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        printf("Usage: %s <schedule_trace.csv>\n", argv[0]);
        return 1;
    }

    if (load_csv(argv[1]) != 0) {
        fprintf(stderr, "Failed to load trace file: %s\n", argv[1]);
        return 1;
    }

    qsort(records, record_count, sizeof(TraceRecord), cmp_timestamp);
    calculate_stats();

    print_timeline();
    print_summary();

    return 0;
}
